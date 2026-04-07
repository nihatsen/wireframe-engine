#pragma once

#include <string>
#include <atomic>
#include <set>
#include <map>

// ═══════════════════════════════════════════════════════════════
//  CONFIGURATION MANAGER
//  Reads config from file with hot-reload support
//  Supports per-app profiles for game-specific tuning
// ═══════════════════════════════════════════════════════════════

namespace config {

enum class RenderMode : int {
    OFF = 0,
    WIREFRAME = 1,         // White edges on black
    WIREFRAME_OVERLAY = 2, // Edges over original
    INVERTED = 3,          // Black edges on white
    COLORED = 4,           // Edges with original color
    XRAY = 5,             // Transparent + edges
    GAME_WIREFRAME = 6    // Game-optimized: multi-channel + depth edges
};

enum class ShaderQuality : int {
    ROBERTS = 0,    // Fastest
    SOBEL = 1,      // Balanced
    SCHARR = 2,     // High quality
    FREICHEN = 3    // Highest quality
};

// Per-app profile overrides (unset values use global defaults)
struct AppProfile {
    std::string package_name;
    int mode = -1;              // -1 = use global
    int quality = -1;           // -1 = use global
    float edge_threshold = -1;  // <0 = use global
    float smooth_min = -1;
    float smooth_max = -1;
    float line_color[3] = {-1, -1, -1};
    float bg_color[3] = {-1, -1, -1};
    float bg_opacity = -1;
    float line_intensity = -1;
    float resolution_scale = -1;
    int frame_skip = -1;
    bool use_half_float_set = false;
    bool use_half_float = false;
    bool use_compute_set = false;
    bool use_compute = false;
};

struct Config {
    // Core
    bool enabled = true;
    bool video_wireframe = true;   // MediaCodec video wireframe
    RenderMode mode = RenderMode::WIREFRAME;

    // Edge detection
    ShaderQuality quality = ShaderQuality::SOBEL;
    float edge_threshold = 0.08f;
    float smooth_min = 0.05f;
    float smooth_max = 0.20f;

    // Visual
    float line_color[3] = {1.0f, 1.0f, 1.0f};
    float bg_color[3] = {0.0f, 0.0f, 0.0f};
    float bg_opacity = 0.0f;
    float line_intensity = 1.2f;

    // Performance
    float resolution_scale = 1.0f;
    bool use_compute = false;
    int frame_skip = 0;
    bool use_half_float = false;
    bool use_pbo = false;
    bool cache_shaders = true;

    // Scope
    bool apply_all = true;
    bool process_systemui = true;
    bool process_launcher = true;
    bool process_keyboard = true;
    std::set<std::string> blacklist;
    std::set<std::string> whitelist;

    // Per-app profiles
    std::map<std::string, AppProfile> profiles;

    // Debug
    bool debug_logging = false;
    bool show_fps = false;
    bool dump_shaders = false;
};

// Initialize configuration system
void init();

// Get current config (thread-safe)
const Config& get();

// Get effective config for a specific package (applies profile overrides)
Config get_effective(const std::string& package_name);

// Reload configuration from file
void reload();

// Check if a package should be processed
bool should_process(const std::string& package_name);

// Check if config was updated since last check
bool was_updated();

// Set the active package name (called once at startup)
void set_package(const std::string& package_name);

// Get the active package name
const std::string& get_package();

// Config file paths
constexpr const char* SYSTEM_CONFIG_PATH = "/system/etc/wireframe/config.conf";
constexpr const char* RUNTIME_CONFIG_PATH = "/data/local/tmp/wireframe/config.conf";

} // namespace config
