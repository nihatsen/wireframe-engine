#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2.h>

#include <unordered_map>
#include <mutex>
#include <cstdint>

// ═══════════════════════════════════════════════════════════════
//  WIREFRAME RENDERER
//  GPU-accelerated edge detection post-processing pipeline
// ═══════════════════════════════════════════════════════════════

namespace renderer {

// Per-EGL-context rendering state
struct ContextState {
    // OpenGL resources
    GLuint program = 0;
    GLuint fbo = 0;
    GLuint texture = 0;
    GLuint vbo = 0;
    GLuint vao = 0;

    // For downscaled processing
    GLuint downscale_fbo = 0;
    GLuint downscale_texture = 0;

    // PBO for async copy
    GLuint pbo[2] = {0, 0};
    int pbo_index = 0;

    // Compute shader resources
    GLuint compute_program = 0;
    GLuint output_texture = 0;

    // Uniform locations
    GLint u_texture = -1;
    GLint u_texel_size = -1;
    GLint u_threshold = -1;
    GLint u_smooth_min = -1;
    GLint u_smooth_max = -1;
    GLint u_line_color = -1;
    GLint u_bg_color = -1;
    GLint u_bg_opacity = -1;
    GLint u_intensity = -1;

    // Dimensions
    int width = 0;
    int height = 0;
    int proc_width = 0;  // Processing width (may be downscaled)
    int proc_height = 0;

    // State
    bool initialized = false;
    bool needs_rebuild = false;
    int gles_version = 2;
    uint64_t frame_count = 0;
    uint64_t config_version = 0;

    // Performance tracking
    double last_frame_time = 0;
    double avg_frame_time = 0;
};

// Initialize the renderer system
void init();

// Process a frame before eglSwapBuffers
// Called with the current EGL context active
void process_frame(EGLDisplay display, EGLSurface surface);

// Cleanup resources for a context being destroyed
void cleanup_context(EGLContext context);

// Cleanup all resources
void shutdown();

// Get FPS stats
float get_fps();
float get_frame_time_ms();

} // namespace renderer
