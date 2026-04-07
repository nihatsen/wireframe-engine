// ═══════════════════════════════════════════════════════════════
//  WIREFRAME RENDER ENGINE - Zygisk Module Entry Point
//  Hooks eglSwapBuffers system-wide via PLT/GOT patching
// ═══════════════════════════════════════════════════════════════

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

// Zygisk API
#include "zygisk.hpp"

// Our modules
#include "hook.h"
#include "renderer.h"
#include "config.h"
#include "video_hook.h"

#include <EGL/egl.h>

#define LOG_TAG "WireframeZygisk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── EGL Hook Functions ─────────────────────────────────────

// Original function pointers
static EGLBoolean (*original_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static EGLBoolean (*original_eglSwapBuffersWithDamage)(EGLDisplay, EGLSurface, const EGLint*, EGLint) = nullptr;
static EGLContext (*original_eglDestroyContext)(EGLDisplay, EGLContext) = nullptr;

static bool g_hooks_installed = false;
static std::string g_package_name;
static JavaVM* g_jvm = nullptr;

// Forward declarations
static EGLBoolean hooked_eglSwapBuffersWithDamage(
        EGLDisplay display, EGLSurface surface,
        const EGLint* rects, EGLint n_rects);

// ─── Hooked eglSwapBuffers ──────────────────────────────────
static EGLBoolean hooked_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    // Video render threads have their own wireframe pipeline — skip main hook
    // to avoid double-wireframing and wasted GPU work.
    if (!video_hook::is_video_thread()) {
        renderer::process_frame(display, surface);
    }

    // Call original (via inline hook trampoline)
    return original_eglSwapBuffers(display, surface);
}

// ─── Hooked eglSwapBuffersWithDamageKHR ─────────────────────
static EGLBoolean hooked_eglSwapBuffersWithDamage(
        EGLDisplay display, EGLSurface surface,
        const EGLint* rects, EGLint n_rects) {

    if (!video_hook::is_video_thread()) {
        renderer::process_frame(display, surface);
    }

    return original_eglSwapBuffersWithDamage(display, surface, rects, n_rects);
}

// ─── Hooked eglDestroyContext ───────────────────────────────
static EGLContext hooked_eglDestroyContext(EGLDisplay display, EGLContext context) {
    renderer::cleanup_context(context);
    return original_eglDestroyContext(display, context);
}

// ─── Install Hooks ──────────────────────────────────────────
static void install_hooks() {
    if (g_hooks_installed) return;

    LOGI("Installing EGL hooks for %s", g_package_name.c_str());

    // Wait for libEGL to be loaded
    void* egl_handle = dlopen("libEGL.so", RTLD_NOLOAD);
    if (!egl_handle) {
        // Try to load it
        egl_handle = dlopen("libEGL.so", RTLD_NOW);
    }
    if (!egl_handle) {
        LOGE("Failed to find libEGL.so");
        return;
    }

    // Get original function addresses via dlsym
    auto* real_swap = reinterpret_cast<EGLBoolean(*)(EGLDisplay, EGLSurface)>(
        dlsym(egl_handle, "eglSwapBuffers"));

    if (!real_swap) {
        LOGE("Failed to find eglSwapBuffers");
        return;
    }

    // Strategy: Use INLINE hook on libEGL.so's exported eglSwapBuffers.
    // This catches ALL callers — even game engines that resolve via dlsym()
    // (e.g. Brawl Stars' libg.so, Unity's libunity.so).
    // PLT hooks only catch callers that import via GOT, missing dlsym users.
    void* swap_trampoline = nullptr;
    int result = hook::inline_hook(
        reinterpret_cast<void*>(real_swap),
        reinterpret_cast<void*>(hooked_eglSwapBuffers),
        &swap_trampoline);

    if (result == 0 && swap_trampoline) {
        original_eglSwapBuffers = reinterpret_cast<EGLBoolean(*)(EGLDisplay, EGLSurface)>(
            swap_trampoline);
        LOGI("Inline hook installed for eglSwapBuffers");
    } else {
        LOGW("Inline hook failed (%d), falling back to PLT hook", result);
        // Fallback to PLT hooking
        result = hook::hook_plt(
            "eglSwapBuffers",
            reinterpret_cast<void*>(hooked_eglSwapBuffers),
            reinterpret_cast<void**>(&original_eglSwapBuffers));
        if (result != 0) {
            original_eglSwapBuffers = real_swap;
        }
    }

    // Hook eglSwapBuffersWithDamageKHR via inline hook
    auto* real_swap_damage = reinterpret_cast<void*>(
        eglGetProcAddress("eglSwapBuffersWithDamageKHR"));
    if (!real_swap_damage) {
        real_swap_damage = dlsym(egl_handle, "eglSwapBuffersWithDamageKHR");
    }
    if (real_swap_damage) {
        void* damage_trampoline = nullptr;
        int r = hook::inline_hook(
            real_swap_damage,
            reinterpret_cast<void*>(hooked_eglSwapBuffersWithDamage),
            &damage_trampoline);
        if (r == 0 && damage_trampoline) {
            original_eglSwapBuffersWithDamage =
                reinterpret_cast<EGLBoolean(*)(EGLDisplay, EGLSurface, const EGLint*, EGLint)>(
                    damage_trampoline);
            LOGI("Inline hook installed for eglSwapBuffersWithDamageKHR");
        } else {
            // Fallback to PLT
            hook::hook_plt(
                "eglSwapBuffersWithDamageKHR",
                reinterpret_cast<void*>(hooked_eglSwapBuffersWithDamage),
                reinterpret_cast<void**>(&original_eglSwapBuffersWithDamage));
            if (!original_eglSwapBuffersWithDamage) {
                original_eglSwapBuffersWithDamage =
                    reinterpret_cast<EGLBoolean(*)(EGLDisplay, EGLSurface, const EGLint*, EGLint)>(
                        real_swap_damage);
            }
        }
    }

    // Hook eglDestroyContext for cleanup (PLT is fine — no game calls this via dlsym)
    hook::hook_plt(
        "eglDestroyContext",
        reinterpret_cast<void*>(hooked_eglDestroyContext),
        reinterpret_cast<void**>(&original_eglDestroyContext));

    g_hooks_installed = true;
    LOGI("EGL hooks installed successfully");

    // Video wireframing via MediaCodec surface redirect
    if (g_jvm) {
        video_hook::init(g_jvm);
    }
}

// ═══════════════════════════════════════════════════════════════
//  ZYGISK MODULE CLASS
// ═══════════════════════════════════════════════════════════════

class WireframeModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api_ = api;
        this->env_ = env;
        // Save JavaVM for video hook JNI operations
        env->GetJavaVM(&g_jvm);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        // Get the package name
        if (args->nice_name) {
            const char* name = env_->GetStringUTFChars(args->nice_name, nullptr);
            if (name) {
                g_package_name = name;
                env_->ReleaseStringUTFChars(args->nice_name, name);
            }
        }

        // Check system property for master enable
        char prop[PROP_VALUE_MAX] = {0};
        __system_property_get("persist.wireframe.enabled", prop);
        if (strcmp(prop, "0") == 0) {
            LOGI("Wireframe disabled via property, skipping %s", g_package_name.c_str());
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Initialize config to check if this process should be hooked
        config::init();

        if (!config::should_process(g_package_name)) {
            LOGI("Skipping %s (not in scope)", g_package_name.c_str());
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        LOGI("Will hook process: %s", g_package_name.c_str());
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (g_package_name.empty()) return;

        if (!config::should_process(g_package_name)) return;

        // Set the active package so renderer can apply per-app profiles
        config::set_package(g_package_name);

        // Initialize renderer
        renderer::init();

        // Install hooks as soon as libEGL.so is loaded.
        // Poll with minimal delay — fast apps (launcher, systemui) render their
        // first frames within milliseconds of postAppSpecialize.
        std::thread([]() {
            for (int attempt = 0; attempt < 50; attempt++) {
                void* egl = dlopen("libEGL.so", RTLD_NOLOAD);
                if (egl) {
                    dlclose(egl);
                    install_hooks();
                    return;
                }
                usleep(50000); // 50ms between attempts (2.5s total window)
            }
            LOGW("libEGL.so not loaded after timeout, hooks not installed");
        }).detach();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs* args) override {
        // system_server — hook for SystemUI
        g_package_name = "system_server";

        config::init();
        if (!config::get().enabled) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        LOGI("Hooking system_server for system-wide wireframe");
    }

    void postServerSpecialize(const zygisk::ServerSpecializeArgs* args) override {
        if (!config::get().enabled) return;

        renderer::init();

        std::thread([]() {
            usleep(2000000); // 2 seconds for system_server
            install_hooks();
        }).detach();
    }

private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
};

// Register the Zygisk module
REGISTER_ZYGISK_MODULE(WireframeModule)
