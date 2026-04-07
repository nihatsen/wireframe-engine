// ═══════════════════════════════════════════════════════════════
//  VIDEO WIREFRAME — MediaCodec Surface Redirect
//  Hooks MediaCodec::configure in libstagefright.so to redirect
//  video decoder output through SurfaceTexture → Sobel shader →
//  original Surface. GPU-only pipeline, ~0.5ms/frame.
// ═══════════════════════════════════════════════════════════════

#include "video_hook.h"
#include "hook.h"
#include "config.h"
#include "shaders.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <elf.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <jni.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#define LOG_TAG "WireframeVideo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Thread-local flag: video render threads skip wireframe in main hook
static thread_local bool s_is_video_thread = false;

static JavaVM* s_jvm = nullptr;

// Reentrancy guard (4-param configure calls 5-param internally)
static thread_local bool s_in_configure = false;

// ANativeWindow offset within Surface object (computed at runtime)
static ptrdiff_t s_anw_offset = 0;

// ANativeWindow magic: '_wnd' = 0x5f776e64
static constexpr uint32_t ANW_MAGIC = 0x5f776e64;

// ─── Vertex shader with SurfaceTexture transform matrix ────────
static constexpr const char* VERTEX_ST = R"glsl(
    precision highp float;
    attribute vec2 aPosition;
    varying vec2 vTexCoord;
    uniform mat4 uSTMatrix;

    void main() {
        gl_Position = vec4(aPosition, 0.0, 1.0);
        vec2 tc = aPosition * 0.5 + 0.5;
        vTexCoord = (uSTMatrix * vec4(tc, 0.0, 1.0)).xy;
    }
)glsl";

// ─── Per-video-surface state ───────────────────────────────────
struct VideoSurface {
    ANativeWindow* original_anw;     // Original ANativeWindow (offset-corrected)
    void* codec_surface_ptr;         // Our Surface* (native) for sp<Surface> swap

    // Java objects (global refs)
    jobject java_st;                 // android.graphics.SurfaceTexture
    jobject java_surf;               // android.view.Surface (backed by our ST)
    jobject java_matrix;             // float[16] reusable array

    // Cached JNI method IDs
    jmethodID mid_updateTexImage;
    jmethodID mid_getTransformMatrix;
    jmethodID mid_getTimestamp;
    jmethodID mid_attachToGLContext;

    // GL/EGL resources (initialized on render thread)
    GLuint oes_texture;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;

    GLuint shader_program;
    GLuint vbo;
    GLint u_texture;
    GLint u_texel_size;
    GLint u_st_matrix;
    GLint u_smooth_min;
    GLint u_smooth_max;
    GLint u_line_color;
    GLint u_bg_color;
    GLint u_intensity;

    std::atomic<bool> running{true};
    std::thread render_thread;
};

static std::mutex s_surfaces_mutex;
static std::vector<VideoSurface*> s_surfaces;

// Original function pointers (via inline hook trampolines)
typedef int (*configure_4_t)(void*, void*, void*, void*, uint32_t);
static configure_4_t original_configure_4 = nullptr;
typedef int (*configure_5_t)(void*, void*, void*, void*, void*, uint32_t);
static configure_5_t original_configure_5 = nullptr;
// setSurface(const sp<Surface>&) — called by setOutputSurface
typedef int (*set_surface_t)(void*, void*);
static set_surface_t original_set_surface = nullptr;

// ─── Memory readability check (pipe trick) ─────────────────────
static bool is_readable(const void* addr, size_t len) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return false;
    ssize_t ret = write(pipefd[1], addr, len);
    close(pipefd[0]);
    close(pipefd[1]);
    return ret == static_cast<ssize_t>(len);
}

// ─── Find symbol in mapped ELF by parsing /proc/self/maps ─────
static void* find_symbol_in_mapped_lib(const char* lib_name, const char* sym_name) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return nullptr;

    uintptr_t lib_base = 0;
    char line[512];

    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, lib_name) && strstr(line, " r--p ")) {
            if (sscanf(line, "%" SCNxPTR, &lib_base) == 1) {
                break;
            }
        }
    }
    fclose(maps);

    if (!lib_base) {
        LOGW("Library %s not found in /proc/self/maps", lib_name);
        return nullptr;
    }

    LOGI("Found %s at base 0x%" PRIxPTR, lib_name, lib_base);

    if (!is_readable(reinterpret_cast<void*>(lib_base), sizeof(Elf64_Ehdr))) {
        LOGE("Cannot read ELF header at 0x%" PRIxPTR, lib_base);
        return nullptr;
    }

#ifdef __LP64__
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(lib_base);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        LOGE("Bad ELF magic at 0x%" PRIxPTR, lib_base);
        return nullptr;
    }

    if (!is_readable(reinterpret_cast<void*>(lib_base + ehdr->e_phoff),
                     ehdr->e_phnum * sizeof(Elf64_Phdr))) {
        LOGE("Cannot read program headers");
        return nullptr;
    }

    auto* phdr = reinterpret_cast<Elf64_Phdr*>(lib_base + ehdr->e_phoff);
    Elf64_Dyn* dyn = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<Elf64_Dyn*>(lib_base + phdr[i].p_offset);
            break;
        }
    }
    if (!dyn) {
        LOGE("No PT_DYNAMIC found");
        return nullptr;
    }

    Elf64_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    size_t strtab_size = 0;

    if (!is_readable(dyn, sizeof(Elf64_Dyn) * 2)) {
        LOGE("Cannot read dynamic section");
        return nullptr;
    }

    uintptr_t raw_symtab = 0, raw_strtab = 0;
    for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        if (!is_readable(d + 1, sizeof(Elf64_Dyn))) break;
        switch (d->d_tag) {
            case DT_SYMTAB: raw_symtab = d->d_un.d_ptr; break;
            case DT_STRTAB: raw_strtab = d->d_un.d_ptr; break;
            case DT_STRSZ:  strtab_size = d->d_un.d_val; break;
        }
    }

    if (!raw_symtab || !raw_strtab) {
        LOGE("No symtab/strtab in dynamic section");
        return nullptr;
    }

    if (raw_symtab < lib_base) {
        symtab = reinterpret_cast<Elf64_Sym*>(lib_base + raw_symtab);
        strtab = reinterpret_cast<const char*>(lib_base + raw_strtab);
    } else {
        symtab = reinterpret_cast<Elf64_Sym*>(raw_symtab);
        strtab = reinterpret_cast<const char*>(raw_strtab);
    }

    if (!is_readable(symtab, sizeof(Elf64_Sym)) || !is_readable(strtab, 16)) {
        LOGE("symtab/strtab not readable");
        return nullptr;
    }

    size_t sym_count = 0;
    if (reinterpret_cast<uintptr_t>(strtab) > reinterpret_cast<uintptr_t>(symtab)) {
        sym_count = (reinterpret_cast<uintptr_t>(strtab) -
                     reinterpret_cast<uintptr_t>(symtab)) / sizeof(Elf64_Sym);
    }
    if (sym_count == 0 || sym_count > 100000) sym_count = 10000;

    size_t target_len = strlen(sym_name);
    for (size_t i = 0; i < sym_count; i++) {
        if (symtab[i].st_name == 0) continue;
        if (ELF64_ST_TYPE(symtab[i].st_info) != STT_FUNC) continue;
        if (symtab[i].st_shndx == SHN_UNDEF) continue;
        if (symtab[i].st_name >= strtab_size) continue;

        const char* name = strtab + symtab[i].st_name;
        if (!is_readable(name, target_len + 1)) continue;

        if (strcmp(name, sym_name) == 0) {
            uintptr_t val = symtab[i].st_value;
            void* addr = (val < lib_base)
                ? reinterpret_cast<void*>(lib_base + val)
                : reinterpret_cast<void*>(val);
            LOGI("Found %s at %p", sym_name, addr);
            return addr;
        }
    }
#else
    // ARM32
    auto* ehdr = reinterpret_cast<Elf32_Ehdr*>(lib_base);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return nullptr;

    auto* phdr = reinterpret_cast<Elf32_Phdr*>(lib_base + ehdr->e_phoff);
    Elf32_Dyn* dyn = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<Elf32_Dyn*>(lib_base + phdr[i].p_offset);
            break;
        }
    }
    if (!dyn) return nullptr;

    Elf32_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    size_t strtab_size = 0;
    uintptr_t raw32_symtab = 0, raw32_strtab = 0;

    for (Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB: raw32_symtab = d->d_un.d_ptr; break;
            case DT_STRTAB: raw32_strtab = d->d_un.d_ptr; break;
            case DT_STRSZ:  strtab_size = d->d_un.d_val;  break;
        }
    }
    if (!raw32_symtab || !raw32_strtab) return nullptr;

    if (raw32_symtab < lib_base) {
        symtab = reinterpret_cast<Elf32_Sym*>(lib_base + raw32_symtab);
        strtab = reinterpret_cast<const char*>(lib_base + raw32_strtab);
    } else {
        symtab = reinterpret_cast<Elf32_Sym*>(raw32_symtab);
        strtab = reinterpret_cast<const char*>(raw32_strtab);
    }

    size_t sym_count = 0;
    if (reinterpret_cast<uintptr_t>(strtab) > reinterpret_cast<uintptr_t>(symtab)) {
        sym_count = (reinterpret_cast<uintptr_t>(strtab) -
                     reinterpret_cast<uintptr_t>(symtab)) / sizeof(Elf32_Sym);
    }
    if (sym_count == 0 || sym_count > 100000) sym_count = 10000;

    for (size_t i = 0; i < sym_count; i++) {
        if (symtab[i].st_name == 0) continue;
        if (ELF32_ST_TYPE(symtab[i].st_info) != STT_FUNC) continue;
        if (symtab[i].st_shndx == SHN_UNDEF) continue;
        if (symtab[i].st_name >= strtab_size) continue;

        const char* name = strtab + symtab[i].st_name;
        if (strcmp(name, sym_name) == 0) {
            uintptr_t val = symtab[i].st_value;
            return (val < lib_base)
                ? reinterpret_cast<void*>(lib_base + val)
                : reinterpret_cast<void*>(val);
        }
    }
#endif

    LOGW("Symbol %s not found in %s", sym_name, lib_name);
    return nullptr;
}

// ─── GL helpers ────────────────────────────────────────────────
static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader compile: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint create_program(const char* vs, const char* fs) {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    if (!v) return 0;
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!f) { glDeleteShader(v); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "aPosition");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("Program link: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ─── Get JNIEnv for current thread ─────────────────────────────
static JNIEnv* get_env() {
    if (!s_jvm) return nullptr;
    JNIEnv* env = nullptr;
    int status = s_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) return env;
    if (status == JNI_EDETACHED) {
        if (s_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) return env;
    }
    return nullptr;
}

// ─── Find ANativeWindow offset within Surface by scanning for magic ──
// Surface inherits from RefBase (vtable, 16 bytes on 64-bit) then ANativeWindow.
// We scan for the ANativeWindow magic '_wnd' (0x5f776e64) to find the offset.
static ptrdiff_t find_anw_offset_in(void* surface_ptr) {
    if (s_anw_offset > 0) return s_anw_offset;  // Already known

    auto* p = reinterpret_cast<uint8_t*>(surface_ptr);
    for (ptrdiff_t i = 0; i < 128; i += 4) {
        if (is_readable(p + i, 4) &&
            *reinterpret_cast<uint32_t*>(p + i) == ANW_MAGIC) {
            s_anw_offset = i;
            LOGI("ANativeWindow offset in Surface: %td bytes (found magic at %p+%td)",
                 i, surface_ptr, i);
            return i;
        }
    }
    LOGE("ANativeWindow magic not found in Surface at %p", surface_ptr);
    return -1;
}

// ─── Create Video Redirect Pipeline (Java objects only) ────────
// EGL/GL setup is deferred to the render thread.
static VideoSurface* create_video_redirect(void* original_surface_ptr) {
    LOGI("create_video_redirect: starting with surface_ptr=%p", original_surface_ptr);

    // Find ANativeWindow offset if not yet known
    ptrdiff_t offset = find_anw_offset_in(original_surface_ptr);
    if (offset < 0) {
        LOGE("Cannot find ANativeWindow in Surface, aborting redirect");
        return nullptr;
    }

    JNIEnv* env = get_env();
    if (!env) {
        LOGE("Cannot get JNIEnv for video redirect");
        return nullptr;
    }

    // Compute original ANativeWindow from Surface* using the offset
    auto* original_anw = reinterpret_cast<ANativeWindow*>(
        reinterpret_cast<uint8_t*>(original_surface_ptr) + offset);
    ANativeWindow_acquire(original_anw);

    LOGI("create_video_redirect: original_anw=%p (offset=%td, w=%d h=%d)",
         original_anw, s_anw_offset,
         ANativeWindow_getWidth(original_anw),
         ANativeWindow_getHeight(original_anw));

    auto* vs = new VideoSurface();
    vs->original_anw = original_anw;

    // --- Create DETACHED SurfaceTexture (no GL context needed) ---
    jclass stClass = env->FindClass("android/graphics/SurfaceTexture");
    if (!stClass || env->ExceptionCheck()) {
        LOGE("Cannot find SurfaceTexture class");
        if (env->ExceptionCheck()) env->ExceptionClear();
        ANativeWindow_release(original_anw);
        delete vs;
        return nullptr;
    }

    jmethodID stCtor = env->GetMethodID(stClass, "<init>", "(Z)V");
    if (!stCtor || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        stCtor = env->GetMethodID(stClass, "<init>", "(I)V");
        if (!stCtor) {
            ANativeWindow_release(original_anw);
            delete vs;
            return nullptr;
        }
        jobject localST = env->NewObject(stClass, stCtor, static_cast<jint>(0));
        if (!localST || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            ANativeWindow_release(original_anw);
            delete vs;
            return nullptr;
        }
        vs->java_st = env->NewGlobalRef(localST);
        env->DeleteLocalRef(localST);
    } else {
        jobject localST = env->NewObject(stClass, stCtor, static_cast<jboolean>(JNI_FALSE));
        if (!localST || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            ANativeWindow_release(original_anw);
            delete vs;
            return nullptr;
        }
        vs->java_st = env->NewGlobalRef(localST);
        env->DeleteLocalRef(localST);
        LOGI("create_video_redirect: created DETACHED SurfaceTexture");
    }

    // Cache method IDs
    vs->mid_updateTexImage = env->GetMethodID(stClass, "updateTexImage", "()V");
    vs->mid_getTransformMatrix = env->GetMethodID(stClass, "getTransformMatrix", "([F)V");
    vs->mid_getTimestamp = env->GetMethodID(stClass, "getTimestamp", "()J");
    vs->mid_attachToGLContext = env->GetMethodID(stClass, "attachToGLContext", "(I)V");

    jfloatArray localArr = env->NewFloatArray(16);
    vs->java_matrix = env->NewGlobalRef(localArr);
    env->DeleteLocalRef(localArr);
    env->DeleteLocalRef(stClass);

    LOGI("create_video_redirect: SurfaceTexture created, creating Surface...");

    // --- Create Java Surface from SurfaceTexture ---
    jclass surfClass = env->FindClass("android/view/Surface");
    jmethodID surfCtor = env->GetMethodID(surfClass, "<init>",
        "(Landroid/graphics/SurfaceTexture;)V");
    jobject localSurf = env->NewObject(surfClass, surfCtor, vs->java_st);
    if (!localSurf || env->ExceptionCheck()) {
        LOGE("Cannot create Surface from SurfaceTexture");
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteGlobalRef(vs->java_st);
        env->DeleteGlobalRef(vs->java_matrix);
        env->DeleteLocalRef(surfClass);
        ANativeWindow_release(original_anw);
        delete vs;
        return nullptr;
    }
    vs->java_surf = env->NewGlobalRef(localSurf);

    // Get ANativeWindow from our Java Surface
    ANativeWindow* codec_anw = ANativeWindow_fromSurface(env, localSurf);
    env->DeleteLocalRef(localSurf);
    env->DeleteLocalRef(surfClass);

    if (!codec_anw) {
        LOGE("ANativeWindow_fromSurface failed for codec surface");
        env->DeleteGlobalRef(vs->java_st);
        env->DeleteGlobalRef(vs->java_surf);
        env->DeleteGlobalRef(vs->java_matrix);
        ANativeWindow_release(original_anw);
        delete vs;
        return nullptr;
    }

    // Compute Surface* from ANativeWindow* by subtracting the offset
    // (ANativeWindow is at offset bytes from the start of the Surface object)
    vs->codec_surface_ptr = reinterpret_cast<void*>(
        reinterpret_cast<uint8_t*>(codec_anw) - s_anw_offset);
    // Release the extra ref from ANativeWindow_fromSurface
    // (the Java Surface still holds a ref via its native pointer)
    ANativeWindow_release(codec_anw);

    LOGI("create_video_redirect: codec_anw=%p codec_surface_ptr=%p (offset=%td)",
         codec_anw, vs->codec_surface_ptr, s_anw_offset);

    {
        std::lock_guard<std::mutex> lock(s_surfaces_mutex);
        s_surfaces.push_back(vs);
    }

    LOGI("Video redirect created: original_anw=%p codec_surface_ptr=%p",
         original_anw, vs->codec_surface_ptr);
    return vs;
}

// ─── Video Render Thread ───────────────────────────────────────
static void video_render_thread(VideoSurface* vs) {
    s_is_video_thread = true;

    JNIEnv* env = nullptr;
    s_jvm->AttachCurrentThread(&env, nullptr);

    LOGI("Video render thread starting, setting up EGL on original_anw=%p...",
         vs->original_anw);

    // --- Create EGL context + window surface on ORIGINAL window ---
    vs->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(vs->egl_display, &major, &minor)) {
        LOGE("Render thread: eglInitialize failed: 0x%x", eglGetError());
        s_jvm->DetachCurrentThread();
        return;
    }

    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig egl_config;
    EGLint numConfigs;
    if (!eglChooseConfig(vs->egl_display, configAttribs, &egl_config, 1, &numConfigs) ||
        numConfigs == 0) {
        LOGE("Render thread: eglChooseConfig failed");
        s_jvm->DetachCurrentThread();
        return;
    }

    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    vs->egl_context = eglCreateContext(vs->egl_display, egl_config,
                                        EGL_NO_CONTEXT, contextAttribs);
    if (vs->egl_context == EGL_NO_CONTEXT) {
        LOGE("Render thread: eglCreateContext failed: 0x%x", eglGetError());
        s_jvm->DetachCurrentThread();
        return;
    }

    vs->egl_surface = eglCreateWindowSurface(vs->egl_display, egl_config,
                                              vs->original_anw, nullptr);
    if (vs->egl_surface == EGL_NO_SURFACE) {
        LOGE("Render thread: eglCreateWindowSurface failed: 0x%x", eglGetError());
        eglDestroyContext(vs->egl_display, vs->egl_context);
        s_jvm->DetachCurrentThread();
        return;
    }

    if (!eglMakeCurrent(vs->egl_display, vs->egl_surface,
                         vs->egl_surface, vs->egl_context)) {
        LOGE("Render thread: eglMakeCurrent failed: 0x%x", eglGetError());
        eglDestroySurface(vs->egl_display, vs->egl_surface);
        eglDestroyContext(vs->egl_display, vs->egl_context);
        s_jvm->DetachCurrentThread();
        return;
    }

    LOGI("Render thread: EGL context ready, creating OES texture...");

    // --- Create OES texture and attach SurfaceTexture ---
    glGenTextures(1, &vs->oes_texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, vs->oes_texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Attach the detached SurfaceTexture to our GL context
    env->CallVoidMethod(vs->java_st, vs->mid_attachToGLContext,
                         static_cast<jint>(vs->oes_texture));
    if (env->ExceptionCheck()) {
        jthrowable ex = env->ExceptionOccurred();
        env->ExceptionClear();
        jclass exCls = env->GetObjectClass(ex);
        jmethodID msgMid = env->GetMethodID(exCls, "getMessage", "()Ljava/lang/String;");
        auto msg = (jstring)env->CallObjectMethod(ex, msgMid);
        if (msg) {
            const char* cstr = env->GetStringUTFChars(msg, nullptr);
            LOGE("attachToGLContext failed: %s", cstr);
            env->ReleaseStringUTFChars(msg, cstr);
        } else {
            LOGE("attachToGLContext failed (no message)");
        }
        eglMakeCurrent(vs->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(vs->egl_display, vs->egl_surface);
        eglDestroyContext(vs->egl_display, vs->egl_context);
        s_jvm->DetachCurrentThread();
        return;
    }

    LOGI("Render thread: SurfaceTexture attached to GL context (tex=%u)", vs->oes_texture);

    // --- Create shader program ---
    vs->shader_program = create_program(VERTEX_ST, shaders::FRAG_VIDEO_SOBEL);
    if (!vs->shader_program) {
        LOGE("Render thread: failed to create shader program");
        eglMakeCurrent(vs->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(vs->egl_display, vs->egl_surface);
        eglDestroyContext(vs->egl_display, vs->egl_context);
        s_jvm->DetachCurrentThread();
        return;
    }

    vs->u_texture    = glGetUniformLocation(vs->shader_program, "uTexture");
    vs->u_texel_size = glGetUniformLocation(vs->shader_program, "uTexelSize");
    vs->u_st_matrix  = glGetUniformLocation(vs->shader_program, "uSTMatrix");
    vs->u_smooth_min = glGetUniformLocation(vs->shader_program, "uSmoothMin");
    vs->u_smooth_max = glGetUniformLocation(vs->shader_program, "uSmoothMax");
    vs->u_line_color = glGetUniformLocation(vs->shader_program, "uLineColor");
    vs->u_bg_color   = glGetUniformLocation(vs->shader_program, "uBgColor");
    vs->u_intensity  = glGetUniformLocation(vs->shader_program, "uIntensity");

    // --- Create fullscreen quad VBO ---
    {
        static const float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
        glGenBuffers(1, &vs->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vs->vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    LOGI("Render thread: setup complete, entering render loop");

    int64_t last_ts = 0;
    int error_count = 0;
    int frame_count = 0;

    while (vs->running.load()) {
        // Check if video wireframe was disabled via config
        {
            const auto& cfg = config::get();
            if (!cfg.enabled || !cfg.video_wireframe) {
                LOGI("Video wireframe disabled via config, stopping render thread");
                break;
            }
        }

        // Update texture with latest codec output
        env->CallVoidMethod(vs->java_st, vs->mid_updateTexImage);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            error_count++;
            if (error_count > 100) {
                LOGW("Too many updateTexImage errors, stopping render thread");
                break;
            }
            usleep(10000);
            continue;
        }

        jlong ts = env->CallLongMethod(vs->java_st, vs->mid_getTimestamp);
        if (ts != last_ts && ts != 0) {
            last_ts = ts;
            error_count = 0;

            // Get SurfaceTexture transform matrix
            env->CallVoidMethod(vs->java_st, vs->mid_getTransformMatrix,
                                static_cast<jfloatArray>(vs->java_matrix));
            float stMatrix[16];
            env->GetFloatArrayRegion(static_cast<jfloatArray>(vs->java_matrix),
                                     0, 16, stMatrix);

            // Get output surface dimensions
            EGLint width = 0, height = 0;
            eglQuerySurface(vs->egl_display, vs->egl_surface, EGL_WIDTH, &width);
            eglQuerySurface(vs->egl_display, vs->egl_surface, EGL_HEIGHT, &height);
            if (width <= 0 || height <= 0) continue;

            // Get config for wireframe parameters
            const auto& cfg = config::get();

            // Render wireframe
            glViewport(0, 0, width, height);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);

            glUseProgram(vs->shader_program);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, vs->oes_texture);
            glUniform1i(vs->u_texture, 0);

            glUniformMatrix4fv(vs->u_st_matrix, 1, GL_FALSE, stMatrix);
            glUniform2f(vs->u_texel_size, 1.0f / width, 1.0f / height);
            glUniform1f(vs->u_smooth_min, cfg.smooth_min);
            glUniform1f(vs->u_smooth_max, cfg.smooth_max);
            glUniform3fv(vs->u_line_color, 1, cfg.line_color);
            glUniform3fv(vs->u_bg_color, 1, cfg.bg_color);
            glUniform1f(vs->u_intensity, cfg.line_intensity);

            // Draw fullscreen quad
            glBindBuffer(GL_ARRAY_BUFFER, vs->vbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisableVertexAttribArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            // Present to original surface
            if (!eglSwapBuffers(vs->egl_display, vs->egl_surface)) {
                EGLint err = eglGetError();
                LOGW("eglSwapBuffers failed: 0x%x", err);
                if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) {
                    LOGI("Original surface gone, stopping render thread");
                    break;
                }
            }

            frame_count++;
            if (frame_count % 300 == 1) {
                LOGI("Video wireframe: rendered %d frames (%dx%d)", frame_count, width, height);
            }
        }

        // Poll interval: 2ms = very responsive, minimal CPU
        usleep(2000);
    }

    LOGI("Video render thread exiting (rendered %d frames)", frame_count);
    eglMakeCurrent(vs->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(vs->egl_display, vs->egl_surface);
    eglDestroyContext(vs->egl_display, vs->egl_context);
    s_jvm->DetachCurrentThread();
}

// ─── Shared hook logic ─────────────────────────────────────────
// sp<Surface> layout: { Surface* m_ptr; }
struct fake_sp { void* ptr; };

// Stop all existing render threads and clean up their EGL surfaces
// so the native window is free for a new EGL surface.
static void stop_all_render_threads() {
    std::lock_guard<std::mutex> lock(s_surfaces_mutex);
    for (auto* vs : s_surfaces) {
        if (vs->running.load()) {
            LOGI("Stopping previous render thread for anw=%p", vs->original_anw);
            vs->running.store(false);
        }
    }
    // Give render threads time to exit and release EGL surfaces
    if (!s_surfaces.empty()) {
        usleep(50000);  // 50ms
    }
    // Release old entries (render threads are detached, they'll clean up)
    s_surfaces.clear();
}

static VideoSurface* try_redirect(void* surface_sp, uint32_t flags) {
    auto* surf = reinterpret_cast<fake_sp*>(surface_sp);

    LOGI("try_redirect: surface_sp=%p, surf->ptr=%p, flags=%u",
         surface_sp, surf ? surf->ptr : nullptr, flags);

    if (!surf || !surf->ptr) {
        LOGI("try_redirect: no surface, skipping");
        return nullptr;
    }

    // Bit 0 = CONFIGURE_FLAG_ENCODE — skip encoders
    if (flags & 1) {
        LOGI("try_redirect: encoder, skipping");
        return nullptr;
    }

    // Check if wireframe and video wireframe are enabled
    const auto& cfg = config::get();
    if (!cfg.enabled || !cfg.video_wireframe) {
        LOGI("try_redirect: wireframe/video disabled, skipping");
        return nullptr;
    }

    // Stop any existing render threads first — frees the EGL surface
    // so the new render thread can create one on the (potentially same) window
    stop_all_render_threads();

    LOGI("Video decoder configure with surface %p, creating redirect", surf->ptr);

    VideoSurface* vs = create_video_redirect(surf->ptr);
    if (!vs) {
        LOGW("Failed to create video redirect, passing through");
        return nullptr;
    }

    return vs;
}

// ─── Hooked MediaCodec::configure (4-param) ───────────────────
static int hooked_configure_4(void* thiz, void* format_sp,
                               void* surface_sp, void* crypto_sp,
                               uint32_t flags) {
    LOGI("configure-4 ENTRY: thiz=%p surf_sp=%p flags=%u", thiz, surface_sp, flags);

    if (s_in_configure) {
        LOGI("configure-4: reentrant call, passing through");
        return original_configure_4(thiz, format_sp, surface_sp, crypto_sp, flags);
    }

    s_in_configure = true;

    VideoSurface* vs = try_redirect(surface_sp, flags);
    int result;

    if (vs) {
        // In-place swap: replace Surface* in the real sp<Surface>
        auto* surf = reinterpret_cast<fake_sp*>(surface_sp);
        void* saved_ptr = surf->ptr;
        surf->ptr = vs->codec_surface_ptr;  // Our Surface* (proper native pointer)
        LOGI("configure-4: swapped surface %p -> %p, calling original",
             saved_ptr, vs->codec_surface_ptr);

        result = original_configure_4(thiz, format_sp, surface_sp, crypto_sp, flags);

        // Restore original pointer
        surf->ptr = saved_ptr;
        LOGI("configure-4: original returned %d, restored surface ptr", result);

        if (result == 0) {
            vs->render_thread = std::thread(video_render_thread, vs);
            vs->render_thread.detach();
            LOGI("configure-4: render thread started");
        } else {
            LOGW("configure-4: configure failed (%d), cleaning up", result);
            vs->running.store(false);
        }
    } else {
        result = original_configure_4(thiz, format_sp, surface_sp, crypto_sp, flags);
        LOGI("configure-4: original returned %d", result);
    }

    s_in_configure = false;
    return result;
}

// ─── Hooked MediaCodec::configure (5-param) ───────────────────
static int hooked_configure_5(void* thiz, void* format_sp,
                               void* surface_sp, void* crypto_sp,
                               void* descrambler_sp, uint32_t flags) {
    LOGI("configure-5 ENTRY: thiz=%p surf_sp=%p flags=%u", thiz, surface_sp, flags);

    if (s_in_configure) {
        LOGI("configure-5: reentrant call, passing through");
        return original_configure_5(thiz, format_sp, surface_sp, crypto_sp,
                                     descrambler_sp, flags);
    }

    s_in_configure = true;

    VideoSurface* vs = try_redirect(surface_sp, flags);
    int result;

    if (vs) {
        // In-place swap: replace Surface* in the real sp<Surface>
        auto* surf = reinterpret_cast<fake_sp*>(surface_sp);
        void* saved_ptr = surf->ptr;
        surf->ptr = vs->codec_surface_ptr;  // Our Surface* (proper native pointer)
        LOGI("configure-5: swapped surface %p -> %p, calling original",
             saved_ptr, vs->codec_surface_ptr);

        result = original_configure_5(thiz, format_sp, surface_sp, crypto_sp,
                                       descrambler_sp, flags);

        // Restore original pointer
        surf->ptr = saved_ptr;
        LOGI("configure-5: original returned %d, restored surface ptr", result);

        if (result == 0) {
            vs->render_thread = std::thread(video_render_thread, vs);
            vs->render_thread.detach();
            LOGI("configure-5: render thread started");
        } else {
            LOGW("configure-5: configure failed (%d), cleaning up", result);
            vs->running.store(false);
        }
    } else {
        result = original_configure_5(thiz, format_sp, surface_sp, crypto_sp,
                                       descrambler_sp, flags);
        LOGI("configure-5: original returned %d", result);
    }

    s_in_configure = false;
    return result;
}

// ─── Hooked MediaCodec::setSurface ─────────────────────────────
// Called by setOutputSurface() — ExoPlayer uses this to switch surfaces
// without re-creating the codec.
static int hooked_set_surface(void* thiz, void* surface_sp) {
    auto* surf = reinterpret_cast<fake_sp*>(surface_sp);

    LOGI("setSurface ENTRY: thiz=%p surf_sp=%p surf->ptr=%p",
         thiz, surface_sp, surf ? surf->ptr : nullptr);

    if (!surf || !surf->ptr) {
        LOGI("setSurface: no surface, passing through");
        return original_set_surface(thiz, surface_sp);
    }

    // Check if wireframe and video wireframe are enabled
    const auto& cfg = config::get();
    if (!cfg.enabled || !cfg.video_wireframe) {
        return original_set_surface(thiz, surface_sp);
    }

    // Find ANativeWindow offset if not yet known, then check surface size.
    // Skip tiny surfaces (e.g., 1x1 dummy used when video is hidden).
    ptrdiff_t offset = find_anw_offset_in(surf->ptr);
    if (offset >= 0) {
        auto* anw = reinterpret_cast<ANativeWindow*>(
            reinterpret_cast<uint8_t*>(surf->ptr) + offset);
        int w = ANativeWindow_getWidth(anw);
        int h = ANativeWindow_getHeight(anw);
        if (w <= 1 || h <= 1) {
            LOGI("setSurface: tiny surface (%dx%d), skipping redirect", w, h);
            return original_set_surface(thiz, surface_sp);
        }
    }

    // Stop old render threads and create a new redirect
    stop_all_render_threads();

    VideoSurface* vs = create_video_redirect(surf->ptr);
    if (!vs) {
        LOGW("setSurface: redirect failed, passing through");
        return original_set_surface(thiz, surface_sp);
    }

    // In-place swap
    void* saved_ptr = surf->ptr;
    surf->ptr = vs->codec_surface_ptr;
    LOGI("setSurface: swapped surface %p -> %p, calling original",
         saved_ptr, vs->codec_surface_ptr);

    int result = original_set_surface(thiz, surface_sp);

    // Restore
    surf->ptr = saved_ptr;
    LOGI("setSurface: original returned %d", result);

    if (result == 0) {
        vs->render_thread = std::thread(video_render_thread, vs);
        vs->render_thread.detach();
        LOGI("setSurface: render thread started");
    } else {
        LOGW("setSurface: failed (%d), cleaning up", result);
        vs->running.store(false);
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════

namespace video_hook {

void init(JavaVM* vm) {
    s_jvm = vm;
    LOGI("Video hook init starting (MediaCodec redirect approach)");

    // ANativeWindow offset will be computed lazily on first redirect attempt

    // Find MediaCodec::configure in libstagefright.so
    void* target_5 = find_symbol_in_mapped_lib(
        "libstagefright.so",
        "_ZN7android10MediaCodec9configureERKNS_2spINS_8AMessageEEE"
        "RKNS1_INS_7SurfaceEEERKNS1_INS_7ICryptoEEERKNS1_INS_8hard"
        "ware3cas6native4V1_012IDescramblerEEEj");

    void* target_4 = find_symbol_in_mapped_lib(
        "libstagefright.so",
        "_ZN7android10MediaCodec9configureERKNS_2spINS_8AMessageEEE"
        "RKNS1_INS_7SurfaceEEERKNS1_INS_7ICryptoEEEj");

    if (!target_5 && !target_4) {
        LOGW("Could not find MediaCodec::configure, video hook disabled");
        return;
    }

    if (target_5) {
        void* trampoline = nullptr;
        int r = hook::inline_hook(target_5,
                                   reinterpret_cast<void*>(hooked_configure_5),
                                   &trampoline);
        if (r == 0 && trampoline) {
            original_configure_5 = reinterpret_cast<configure_5_t>(trampoline);
            LOGI("Hooked MediaCodec::configure (5-param) at %p", target_5);
        } else {
            LOGW("Failed to hook 5-param configure (err=%d)", r);
        }
    }

    if (target_4) {
        void* trampoline = nullptr;
        int r = hook::inline_hook(target_4,
                                   reinterpret_cast<void*>(hooked_configure_4),
                                   &trampoline);
        if (r == 0 && trampoline) {
            original_configure_4 = reinterpret_cast<configure_4_t>(trampoline);
            LOGI("Hooked MediaCodec::configure (4-param) at %p", target_4);
        } else {
            LOGW("Failed to hook 4-param configure (err=%d)", r);
        }
    }

    // Hook setSurface (used by setOutputSurface for surface switching)
    // android::MediaCodec::setSurface(const sp<Surface>&)
    void* target_ss = find_symbol_in_mapped_lib(
        "libstagefright.so",
        "_ZN7android10MediaCodec10setSurfaceERKNS_2spINS_7SurfaceEEE");

    if (target_ss) {
        void* trampoline = nullptr;
        int r = hook::inline_hook(target_ss,
                                   reinterpret_cast<void*>(hooked_set_surface),
                                   &trampoline);
        if (r == 0 && trampoline) {
            original_set_surface = reinterpret_cast<set_surface_t>(trampoline);
            LOGI("Hooked MediaCodec::setSurface at %p", target_ss);
        } else {
            LOGW("Failed to hook setSurface (err=%d)", r);
        }
    } else {
        LOGW("Could not find MediaCodec::setSurface");
    }

    LOGI("Video hook init complete");
}

bool is_video_thread() {
    return s_is_video_thread;
}

} // namespace video_hook
