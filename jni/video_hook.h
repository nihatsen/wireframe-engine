#pragma once

// ═══════════════════════════════════════════════════════════════
//  VIDEO WIREFRAME — MediaCodec Surface Redirect
//  Hooks MediaCodec::configure in libstagefright.so to redirect
//  video decoder output through SurfaceTexture + wireframe shader.
// ═══════════════════════════════════════════════════════════════

#include <jni.h>

namespace video_hook {

// Initialize video hooking. Requires JavaVM for JNI operations.
void init(JavaVM* vm);

// Returns true if calling thread is a video render thread.
// Used by main eglSwapBuffers hook to skip double-wireframing.
bool is_video_thread();

} // namespace video_hook
