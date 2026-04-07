#include "renderer.h"
#include "shaders.h"
#include "config.h"

#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2.h>

#include <unordered_map>
#include <mutex>
#include <cstring>
#include <cmath>
#include <chrono>

#define LOG_TAG "WireframeGPU"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef NDEBUG
#define GL_CHECK(x) x
#else
#define GL_CHECK(x) do { \
    x; \
    GLenum err = glGetError(); \
    if (err != GL_NO_ERROR) \
        LOGE("GL error %d at %s:%d", err, __FILE__, __LINE__); \
} while(0)
#endif

namespace renderer {

// Key: {EGLContext, width, height} — one context may render to multiple surface sizes
struct ContextKey {
    EGLContext context;
    int width;
    int height;
    bool operator==(const ContextKey& o) const {
        return context == o.context && width == o.width && height == o.height;
    }
};
struct ContextKeyHash {
    size_t operator()(const ContextKey& k) const {
        size_t h = std::hash<void*>()(k.context);
        h ^= std::hash<int>()(k.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<ContextKey, ContextState, ContextKeyHash> s_contexts;
static std::mutex s_context_mutex;
static std::atomic<float> s_fps{0.0f};
static std::atomic<float> s_frame_time_ms{0.0f};

// ─── GL State Save/Restore ──────────────────────────────────
struct GLStateSave {
    GLint program;
    GLint active_texture;
    GLint texture_2d_unit0;   // GL_TEXTURE0 binding — we always write this
    GLint texture_2d_active;  // binding on whatever unit was active
    GLint fbo_draw;
    GLint fbo_read;
    GLint viewport[4];
    GLint vbo;
    GLint vao;
    GLboolean blend;
    GLboolean depth_test;
    GLboolean scissor_test;
    GLboolean cull_face;
    GLboolean stencil_test;
    GLint blend_src_rgb, blend_dst_rgb;
    GLint blend_src_alpha, blend_dst_alpha;
    // Write masks — UE4 and other engines set these; we must not leave them changed
    GLboolean color_mask[4];
    GLboolean depth_mask;
    // Pixel pack alignment (glBlitFramebuffer path doesn't touch this, but be safe)
    GLint pack_alignment;
    GLint unpack_alignment;

    void save() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        // Save GL_TEXTURE0 binding explicitly (we always bind there)
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d_unit0);
        // Restore active unit, then save its binding
        glActiveTexture(active_texture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d_active);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo_draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fbo_read);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        blend        = glIsEnabled(GL_BLEND);
        depth_test   = glIsEnabled(GL_DEPTH_TEST);
        scissor_test = glIsEnabled(GL_SCISSOR_TEST);
        cull_face    = glIsEnabled(GL_CULL_FACE);
        stencil_test = glIsEnabled(GL_STENCIL_TEST);
        glGetIntegerv(GL_BLEND_SRC_RGB,   &blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB,   &blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glGetIntegerv(GL_PACK_ALIGNMENT,   &pack_alignment);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    }

    void restore() {
        glUseProgram(program);
        // Restore GL_TEXTURE0 first (we modified it)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_2d_unit0);
        // Restore the originally active unit and its binding
        glActiveTexture(active_texture);
        glBindTexture(GL_TEXTURE_2D, texture_2d_active);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_read);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindVertexArray(vao);
        if (blend)        glEnable(GL_BLEND);        else glDisable(GL_BLEND);
        if (depth_test)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
        if (scissor_test) glEnable(GL_SCISSOR_TEST);  else glDisable(GL_SCISSOR_TEST);
        if (cull_face)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
        if (stencil_test) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha);
        glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        glDepthMask(depth_mask);
        glPixelStorei(GL_PACK_ALIGNMENT,   pack_alignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    }
};

// ─── Shader Compilation ─────────────────────────────────────
static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint len;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        if (len > 0) {
            char* log = new char[len];
            glGetShaderInfoLog(shader, len, nullptr, log);
            LOGE("Shader compile error: %s", log);
            LOGE("Shader source:\n%s", source);
            delete[] log;
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_program(const char* vs_source, const char* fs_source) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_source);
    if (!vs) return 0;

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_source);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "aPosition");
    glLinkProgram(program);

    // Shaders can be deleted after linking
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint len;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        if (len > 0) {
            char* log = new char[len];
            glGetProgramInfoLog(program, len, nullptr, log);
            LOGE("Program link error: %s", log);
            delete[] log;
        }
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

// ─── Select Fragment Shader Based on Config ─────────────────
static const char* get_fragment_shader(const config::Config& cfg) {
    switch (cfg.mode) {
        case config::RenderMode::WIREFRAME:
        case config::RenderMode::INVERTED:
            switch (cfg.quality) {
                case config::ShaderQuality::ROBERTS:  return shaders::FRAG_ROBERTS;
                case config::ShaderQuality::SOBEL:    return shaders::FRAG_SOBEL;
                case config::ShaderQuality::SCHARR:   return shaders::FRAG_SCHARR;
                case config::ShaderQuality::FREICHEN: return shaders::FRAG_FREICHEN;
            }
            break;
        case config::RenderMode::WIREFRAME_OVERLAY:
        case config::RenderMode::XRAY:
            return shaders::FRAG_SOBEL_OVERLAY;
        case config::RenderMode::COLORED:
            return shaders::FRAG_SOBEL_COLORED;
        case config::RenderMode::GAME_WIREFRAME:
            return shaders::FRAG_GAME_WIREFRAME;
        default:
            return shaders::FRAG_SOBEL;
    }
    return shaders::FRAG_SOBEL;
}

// ─── Initialize Context Resources ───────────────────────────
static void init_context(ContextState& ctx, int width, int height) {
    const auto& pkg = config::get_package();
    const auto cfg = pkg.empty() ? config::get() : config::get_effective(pkg);

    ctx.width = width;
    ctx.height = height;
    ctx.proc_width = std::max(64, static_cast<int>(width * cfg.resolution_scale));
    ctx.proc_height = std::max(64, static_cast<int>(height * cfg.resolution_scale));

    // Detect GLES version
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (version) {
        if (strstr(version, "3.2")) ctx.gles_version = 32;
        else if (strstr(version, "3.1")) ctx.gles_version = 31;
        else if (strstr(version, "3.0")) ctx.gles_version = 30;
        else ctx.gles_version = 20;
    }

    LOGI("Init context: %dx%d (proc: %dx%d) GLES %d.%d ctx=%p",
         width, height, ctx.proc_width, ctx.proc_height,
         ctx.gles_version / 10, ctx.gles_version % 10,
         eglGetCurrentContext());

    // ─── Create shader program ──────────────────
    // Use ES 1.0 compatible shaders (attribute/varying/texture2D/gl_FragColor)
    // which work on all GLES versions including 3.2.
    // Mixing #version 300 es vertex with versionless fragment causes link errors.
    const char* vs = shaders::VERTEX_SHADER;
    const char* fs = get_fragment_shader(cfg);
    ctx.program = create_program(vs, fs);

    if (!ctx.program) {
        LOGE("Failed to create shader program!");
        return;
    }

    // Get uniform locations
    ctx.u_texture    = glGetUniformLocation(ctx.program, "uTexture");
    ctx.u_texel_size = glGetUniformLocation(ctx.program, "uTexelSize");
    ctx.u_threshold  = glGetUniformLocation(ctx.program, "uThreshold");
    ctx.u_smooth_min = glGetUniformLocation(ctx.program, "uSmoothMin");
    ctx.u_smooth_max = glGetUniformLocation(ctx.program, "uSmoothMax");
    ctx.u_line_color = glGetUniformLocation(ctx.program, "uLineColor");
    ctx.u_bg_color   = glGetUniformLocation(ctx.program, "uBgColor");
    ctx.u_bg_opacity = glGetUniformLocation(ctx.program, "uBgOpacity");
    ctx.u_intensity  = glGetUniformLocation(ctx.program, "uIntensity");

    // ─── Create FBO and texture for framebuffer capture ─────
    GL_CHECK(glGenTextures(1, &ctx.texture));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, ctx.texture));

    GLenum internal_format = cfg.use_half_float ? GL_RGBA16F : GL_RGBA8;
    GLenum type = cfg.use_half_float ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;

    GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
                          ctx.proc_width, ctx.proc_height,
                          0, GL_RGBA, type, nullptr));

    // Use NEAREST for speed (no filtering needed for edge detection input)
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

    GL_CHECK(glGenFramebuffers(1, &ctx.fbo));
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, ctx.fbo));
    GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, ctx.texture, 0));

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("FBO incomplete: 0x%x", status);
    }

    // ─── Create fullscreen quad VBO ─────────────
    static const float quad_vertices[] = {
        // Position (x, y) - forms a triangle strip covering [-1,1]
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };

    GL_CHECK(glGenBuffers(1, &ctx.vbo));
    GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, ctx.vbo));
    GL_CHECK(glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices),
                          quad_vertices, GL_STATIC_DRAW));

    // Create VAO if GLES 3.0+
    if (ctx.gles_version >= 30) {
        GL_CHECK(glGenVertexArrays(1, &ctx.vao));
        GL_CHECK(glBindVertexArray(ctx.vao));
        GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, ctx.vbo));
        GL_CHECK(glEnableVertexAttribArray(0));
        GL_CHECK(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
        GL_CHECK(glBindVertexArray(0));
    }

    // ─── PBO for async framebuffer copy ─────────
    if (cfg.use_pbo && ctx.gles_version >= 30) {
        GL_CHECK(glGenBuffers(2, ctx.pbo));
        size_t buf_size = ctx.proc_width * ctx.proc_height * 4;
        for (int i = 0; i < 2; i++) {
            GL_CHECK(glBindBuffer(GL_PIXEL_PACK_BUFFER, ctx.pbo[i]));
            GL_CHECK(glBufferData(GL_PIXEL_PACK_BUFFER, buf_size, nullptr, GL_STREAM_READ));
        }
        GL_CHECK(glBindBuffer(GL_PIXEL_PACK_BUFFER, 0));
    }

    // ─── Compute shader (ES 3.1+) ──────────────
    if (cfg.use_compute && ctx.gles_version >= 31) {
        GLuint cs = compile_shader(GL_COMPUTE_SHADER, shaders::COMPUTE_SOBEL);
        if (cs) {
            ctx.compute_program = glCreateProgram();
            glAttachShader(ctx.compute_program, cs);
            glLinkProgram(ctx.compute_program);
            glDeleteShader(cs);

            GLint linked;
            glGetProgramiv(ctx.compute_program, GL_LINK_STATUS, &linked);
            if (!linked) {
                LOGW("Compute shader link failed, falling back to fragment");
                glDeleteProgram(ctx.compute_program);
                ctx.compute_program = 0;
            } else {
                // Create output texture for compute
                GL_CHECK(glGenTextures(1, &ctx.output_texture));
                GL_CHECK(glBindTexture(GL_TEXTURE_2D, ctx.output_texture));
                GL_CHECK(glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8,
                                        ctx.proc_width, ctx.proc_height));
                GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
                GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
                LOGI("Compute shader initialized");
            }
        }
    }

    // Unbind everything
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));

    ctx.initialized = true;
    ctx.needs_rebuild = false;
    LOGI("Context initialized successfully");
}

// ─── Destroy Context Resources ──────────────────────────────
static void destroy_context(ContextState& ctx) {
    if (ctx.program) glDeleteProgram(ctx.program);
    if (ctx.compute_program) glDeleteProgram(ctx.compute_program);
    if (ctx.fbo) glDeleteFramebuffers(1, &ctx.fbo);
    if (ctx.downscale_fbo) glDeleteFramebuffers(1, &ctx.downscale_fbo);
    if (ctx.texture) glDeleteTextures(1, &ctx.texture);
    if (ctx.downscale_texture) glDeleteTextures(1, &ctx.downscale_texture);
    if (ctx.output_texture) glDeleteTextures(1, &ctx.output_texture);
    if (ctx.vbo) glDeleteBuffers(1, &ctx.vbo);
    if (ctx.vao) glDeleteVertexArrays(1, &ctx.vao);
    if (ctx.pbo[0]) glDeleteBuffers(2, ctx.pbo);
    ctx = ContextState{};
}

// ─── Process Frame (Fragment Shader Path) ───────────────────
static void process_fragment(ContextState& ctx, const config::Config& cfg) {

    // Step 1: Copy default framebuffer to our FBO
    if (ctx.gles_version >= 30) {
        // Use glBlitFramebuffer (fast GPU-side copy)
        GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, 0));
        GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx.fbo));
        GL_CHECK(glBlitFramebuffer(
            0, 0, ctx.width, ctx.height,
            0, 0, ctx.proc_width, ctx.proc_height,
            GL_COLOR_BUFFER_BIT,
            (ctx.proc_width != ctx.width) ? GL_LINEAR : GL_NEAREST));
    } else {
        // GLES 2.0 fallback: glCopyTexSubImage2D
        GL_CHECK(glBindTexture(GL_TEXTURE_2D, ctx.texture));
        GL_CHECK(glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                      ctx.proc_width, ctx.proc_height));
    }

    // Step 2: Render edge detection to default framebuffer
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GL_CHECK(glViewport(0, 0, ctx.width, ctx.height));

    // Disable everything we don't need
    GL_CHECK(glDisable(GL_BLEND));
    GL_CHECK(glDisable(GL_DEPTH_TEST));
    GL_CHECK(glDisable(GL_SCISSOR_TEST));
    GL_CHECK(glDisable(GL_CULL_FACE));
    GL_CHECK(glDisable(GL_STENCIL_TEST));

    GL_CHECK(glUseProgram(ctx.program));

    // Bind our captured texture
    GL_CHECK(glActiveTexture(GL_TEXTURE0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, ctx.texture));
    GL_CHECK(glUniform1i(ctx.u_texture, 0));

    // Set uniforms
    GL_CHECK(glUniform2f(ctx.u_texel_size,
                         1.0f / ctx.proc_width,
                         1.0f / ctx.proc_height));
    GL_CHECK(glUniform1f(ctx.u_threshold, cfg.edge_threshold));
    GL_CHECK(glUniform1f(ctx.u_smooth_min, cfg.smooth_min));
    GL_CHECK(glUniform1f(ctx.u_smooth_max, cfg.smooth_max));
    GL_CHECK(glUniform1f(ctx.u_intensity, cfg.line_intensity));

    // Handle render mode for colors
    float line_r = cfg.line_color[0];
    float line_g = cfg.line_color[1];
    float line_b = cfg.line_color[2];
    float bg_r = cfg.bg_color[0];
    float bg_g = cfg.bg_color[1];
    float bg_b = cfg.bg_color[2];

    if (cfg.mode == config::RenderMode::INVERTED) {
        // Swap line and bg colors
        std::swap(line_r, bg_r);
        std::swap(line_g, bg_g);
        std::swap(line_b, bg_b);
    }

    GL_CHECK(glUniform3f(ctx.u_line_color, line_r, line_g, line_b));
    GL_CHECK(glUniform3f(ctx.u_bg_color, bg_r, bg_g, bg_b));

    if (ctx.u_bg_opacity >= 0) {
        float opacity = cfg.bg_opacity;
        if (cfg.mode == config::RenderMode::XRAY) opacity = 0.15f;
        else if (cfg.mode == config::RenderMode::GAME_WIREFRAME && opacity <= 0.0f) opacity = 0.15f;
        GL_CHECK(glUniform1f(ctx.u_bg_opacity, opacity));
    }

    // Draw fullscreen quad
    if (ctx.vao && ctx.gles_version >= 30) {
        GL_CHECK(glBindVertexArray(ctx.vao));
        GL_CHECK(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
        GL_CHECK(glBindVertexArray(0));
    } else {
        GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, ctx.vbo));
        GL_CHECK(glEnableVertexAttribArray(0));
        GL_CHECK(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
        GL_CHECK(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
        GL_CHECK(glDisableVertexAttribArray(0));
    }
}

// ─── Process Frame (Compute Shader Path) ────────────────────
static void process_compute(ContextState& ctx, const config::Config& cfg) {

    // Copy framebuffer to input texture
    GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, 0));
    GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx.fbo));
    GL_CHECK(glBlitFramebuffer(
        0, 0, ctx.width, ctx.height,
        0, 0, ctx.proc_width, ctx.proc_height,
        GL_COLOR_BUFFER_BIT,
        (ctx.proc_width != ctx.width) ? GL_LINEAR : GL_NEAREST));

    // Run compute shader
    GL_CHECK(glUseProgram(ctx.compute_program));

    GL_CHECK(glActiveTexture(GL_TEXTURE0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, ctx.texture));
    glUniform1i(glGetUniformLocation(ctx.compute_program, "uInput"), 0);

    GL_CHECK(glBindImageTexture(0, ctx.output_texture, 0, GL_FALSE, 0,
                                 GL_WRITE_ONLY, GL_RGBA8));

    glUniform2f(glGetUniformLocation(ctx.compute_program, "uTexelSize"),
                1.0f / ctx.proc_width, 1.0f / ctx.proc_height);
    glUniform1f(glGetUniformLocation(ctx.compute_program, "uSmoothMin"), cfg.smooth_min);
    glUniform1f(glGetUniformLocation(ctx.compute_program, "uSmoothMax"), cfg.smooth_max);
    glUniform3fv(glGetUniformLocation(ctx.compute_program, "uLineColor"), 1, cfg.line_color);
    glUniform3fv(glGetUniformLocation(ctx.compute_program, "uBgColor"), 1, cfg.bg_color);
    glUniform1f(glGetUniformLocation(ctx.compute_program, "uIntensity"), cfg.line_intensity);

    // Dispatch with 16x16 workgroups
    GLuint groups_x = (ctx.proc_width + 15) / 16;
    GLuint groups_y = (ctx.proc_height + 15) / 16;
    GL_CHECK(glDispatchCompute(groups_x, groups_y, 1));

    GL_CHECK(glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT));

    // Blit compute output to default framebuffer
    // We need to create an FBO for the output texture to blit from it
    GLuint out_fbo;
    GL_CHECK(glGenFramebuffers(1, &out_fbo));
    GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, out_fbo));
    GL_CHECK(glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, ctx.output_texture, 0));
    GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
    GL_CHECK(glBlitFramebuffer(
        0, 0, ctx.proc_width, ctx.proc_height,
        0, 0, ctx.width, ctx.height,
        GL_COLOR_BUFFER_BIT, GL_NEAREST));
    GL_CHECK(glDeleteFramebuffers(1, &out_fbo));
}

// ─── Public API ─────────────────────────────────────────────

void init() {
    config::init();
    LOGI("Wireframe renderer initialized");
}

void process_frame(EGLDisplay display, EGLSurface surface) {
    // Poll config BEFORE the enabled check so enable/disable always takes effect.
    // Use a time-based check (not frame_count) so it fires even when disabled.
    {
        static thread_local int64_t s_last_reload_ms = 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (now_ms - s_last_reload_ms >= 100) {
            s_last_reload_ms = now_ms;
            config::reload();
        }
    }

    // Use per-app effective config (applies profile overrides for games like Brawl Stars)
    const auto& pkg = config::get_package();
    const auto cfg = pkg.empty() ? config::get() : config::get_effective(pkg);

    if (!cfg.enabled || cfg.mode == config::RenderMode::OFF) {
        return;
    }

    // Get current context
    EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return;

    // Get surface dimensions
    EGLint width = 0, height = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);

    if (width <= 0 || height <= 0) return;

    // Skip tiny surfaces (likely offscreen or utility surfaces)
    if (width < 8 || height < 8) return;

    // Skip thin bar surfaces (gesture pill = 1080x66, etc.)
    // Edge detection produces no visible output on these — let original content through.
    if (std::min(width, height) < 70) return;

    // Get or create context state keyed by {context, width, height}
    ContextKey key{context, width, height};
    ContextState* ctx_ptr;
    {
        std::lock_guard<std::mutex> lock(s_context_mutex);
        ctx_ptr = &s_contexts[key];
    }
    ContextState& ctx = *ctx_ptr;

    // Check if we need to (re)initialize
    bool need_init = !ctx.initialized || ctx.needs_rebuild;

    // Also check if our GL program was invalidated (context was destroyed and recreated)
    if (!need_init && ctx.program && !glIsProgram(ctx.program)) {
        need_init = true;
    }

    if (need_init) {
        if (ctx.initialized) {
            destroy_context(ctx);
        }
        init_context(ctx, width, height);
        if (!ctx.initialized) return;
    }

    // Frame skip logic
    ctx.frame_count++;
    if (cfg.frame_skip > 0 && (ctx.frame_count % (cfg.frame_skip + 1)) != 0) {
        return;
    }

    // Trigger context rebuild when config was updated
    if (config::was_updated()) {
        ctx.needs_rebuild = true;
    }

    // Performance timing
    auto start = std::chrono::high_resolution_clock::now();

    // Save GL state
    GLStateSave state;
    state.save();

    // Process frame
    if (cfg.use_compute && ctx.compute_program) {
        process_compute(ctx, cfg);
    } else {
        process_fragment(ctx, cfg);
    }

    // Restore GL state
    state.restore();

    // Update performance stats
    auto end = std::chrono::high_resolution_clock::now();
    double frame_ms = std::chrono::duration<double, std::milli>(end - start).count();
    ctx.avg_frame_time = ctx.avg_frame_time * 0.95 + frame_ms * 0.05;
    s_frame_time_ms.store(static_cast<float>(ctx.avg_frame_time));

    if (ctx.last_frame_time > 0) {
        double elapsed = std::chrono::duration<double>(end.time_since_epoch()).count();
        double dt = elapsed - ctx.last_frame_time;
        if (dt > 0) {
            float fps = 1.0 / dt;
            s_fps.store(s_fps.load() * 0.9f + fps * 0.1f);
        }
    }
    ctx.last_frame_time = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

void cleanup_context(EGLContext context) {
    // When an EGL context is destroyed, invalidate all entries using it.
    // GL resources are implicitly freed by the driver.
    std::lock_guard<std::mutex> lock(s_context_mutex);
    for (auto it = s_contexts.begin(); it != s_contexts.end(); ) {
        if (it->first.context == context) {
            it = s_contexts.erase(it);
        } else {
            ++it;
        }
    }
}

void shutdown() {
    std::lock_guard<std::mutex> lock(s_context_mutex);
    for (auto& [key, state] : s_contexts) {
        destroy_context(state);
    }
    s_contexts.clear();
}

float get_fps() { return s_fps.load(); }
float get_frame_time_ms() { return s_frame_time_ms.load(); }

} // namespace renderer
