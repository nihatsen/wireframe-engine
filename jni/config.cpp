#include "config.h"

#include <android/log.h>
#include <sys/stat.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <cstring>

#define LOG_TAG "WireframeCfg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace config {

static Config s_config;
static std::mutex s_config_mutex;
static std::atomic<bool> s_updated{false};
static time_t s_last_mtime = 0;
static uint64_t s_last_check_frame = 0;
static std::string s_active_package;

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::set<std::string> parse_csv(const std::string& csv) {
    std::set<std::string> result;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto trimmed = trim(item);
        if (!trimmed.empty()) result.insert(trimmed);
    }
    return result;
}

static void apply_profile_key(AppProfile& prof, const std::string& key, const std::string& val) {
    if (key == "mode") prof.mode = std::stoi(val);
    else if (key == "shader_quality") prof.quality = std::stoi(val);
    else if (key == "edge_threshold") prof.edge_threshold = std::stof(val);
    else if (key == "edge_smooth_min") prof.smooth_min = std::stof(val);
    else if (key == "edge_smooth_max") prof.smooth_max = std::stof(val);
    else if (key == "line_color_r") prof.line_color[0] = std::stof(val);
    else if (key == "line_color_g") prof.line_color[1] = std::stof(val);
    else if (key == "line_color_b") prof.line_color[2] = std::stof(val);
    else if (key == "bg_color_r") prof.bg_color[0] = std::stof(val);
    else if (key == "bg_color_g") prof.bg_color[1] = std::stof(val);
    else if (key == "bg_color_b") prof.bg_color[2] = std::stof(val);
    else if (key == "bg_opacity") prof.bg_opacity = std::stof(val);
    else if (key == "line_intensity") prof.line_intensity = std::stof(val);
    else if (key == "resolution_scale") prof.resolution_scale = std::clamp(std::stof(val), 0.25f, 1.0f);
    else if (key == "frame_skip") prof.frame_skip = std::stoi(val);
    else if (key == "use_half_float") { prof.use_half_float_set = true; prof.use_half_float = (val == "1"); }
    else if (key == "use_compute") { prof.use_compute_set = true; prof.use_compute = (val == "1"); }
}

static void parse_config_file(const char* path, Config& cfg) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    std::string current_profile;  // empty = global section

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Check for profile section header: [profile.com.example.app]
        if (line.front() == '[' && line.back() == ']') {
            std::string section = line.substr(1, line.size() - 2);
            const std::string prefix = "profile.";
            if (section.compare(0, prefix.size(), prefix) == 0) {
                current_profile = section.substr(prefix.size());
                // Ensure profile entry exists
                cfg.profiles[current_profile].package_name = current_profile;
            } else {
                current_profile.clear();  // Unknown section, back to global
            }
            continue;
        }

        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq_pos));
        std::string val = trim(line.substr(eq_pos + 1));

        // If inside a profile section, apply to that profile
        if (!current_profile.empty()) {
            apply_profile_key(cfg.profiles[current_profile], key, val);
            // Profile-specific: auto-whitelist the package
            if (cfg.whitelist.find(current_profile) == cfg.whitelist.end()) {
                cfg.whitelist.insert(current_profile);
            }
            continue;
        }

        // Global settings
        if (key == "enabled") cfg.enabled = (val == "1");
        else if (key == "video_wireframe") cfg.video_wireframe = (val == "1");
        else if (key == "mode") cfg.mode = static_cast<RenderMode>(std::stoi(val));
        else if (key == "shader_quality") cfg.quality = static_cast<ShaderQuality>(std::stoi(val));
        else if (key == "edge_threshold") cfg.edge_threshold = std::stof(val);
        else if (key == "edge_smooth_min") cfg.smooth_min = std::stof(val);
        else if (key == "edge_smooth_max") cfg.smooth_max = std::stof(val);
        else if (key == "line_color_r") cfg.line_color[0] = std::stof(val);
        else if (key == "line_color_g") cfg.line_color[1] = std::stof(val);
        else if (key == "line_color_b") cfg.line_color[2] = std::stof(val);
        else if (key == "bg_color_r") cfg.bg_color[0] = std::stof(val);
        else if (key == "bg_color_g") cfg.bg_color[1] = std::stof(val);
        else if (key == "bg_color_b") cfg.bg_color[2] = std::stof(val);
        else if (key == "bg_opacity") cfg.bg_opacity = std::stof(val);
        else if (key == "line_intensity") cfg.line_intensity = std::stof(val);
        else if (key == "resolution_scale") cfg.resolution_scale = std::clamp(std::stof(val), 0.25f, 1.0f);
        else if (key == "use_compute") cfg.use_compute = (val == "1");
        else if (key == "frame_skip") cfg.frame_skip = std::stoi(val);
        else if (key == "use_half_float") cfg.use_half_float = (val == "1");
        else if (key == "use_pbo") cfg.use_pbo = (val == "1");
        else if (key == "cache_shaders") cfg.cache_shaders = (val == "1");
        else if (key == "apply_all") cfg.apply_all = (val == "1");
        else if (key == "process_systemui") cfg.process_systemui = (val == "1");
        else if (key == "process_launcher") cfg.process_launcher = (val == "1");
        else if (key == "process_keyboard") cfg.process_keyboard = (val == "1");
        else if (key == "blacklist") cfg.blacklist = parse_csv(val);
        else if (key == "whitelist") cfg.whitelist = parse_csv(val);
        else if (key == "debug_logging") cfg.debug_logging = (val == "1");
        else if (key == "show_fps_overlay") cfg.show_fps = (val == "1");
        else if (key == "dump_shaders") cfg.dump_shaders = (val == "1");
    }
}

void init() {
    std::lock_guard<std::mutex> lock(s_config_mutex);

    // Load system config first (defaults)
    parse_config_file(SYSTEM_CONFIG_PATH, s_config);

    // Override with runtime config
    parse_config_file(RUNTIME_CONFIG_PATH, s_config);

    LOGI("Config loaded: enabled=%d mode=%d quality=%d scale=%.2f",
         s_config.enabled, (int)s_config.mode,
         (int)s_config.quality, s_config.resolution_scale);

    s_updated.store(true);
}

const Config& get() {
    return s_config;
}

void reload() {
    // Check file modification time to avoid unnecessary reloads
    struct stat st;
    if (stat(RUNTIME_CONFIG_PATH, &st) == 0) {
        if (st.st_mtime != s_last_mtime) {
            s_last_mtime = st.st_mtime;
            init();
        }
    }
}

bool should_process(const std::string& package_name) {
    const auto& cfg = s_config;

    if (!cfg.enabled) return false;

    // Check blacklist
    if (cfg.blacklist.count(package_name) > 0) return false;

    // If not apply_all, check whitelist
    if (!cfg.apply_all) {
        return cfg.whitelist.count(package_name) > 0;
    }

    // Special cases
    if (package_name == "com.android.systemui" && !cfg.process_systemui) return false;
    if (package_name.find("launcher") != std::string::npos && !cfg.process_launcher) return false;
    if (package_name.find("keyboard") != std::string::npos ||
        package_name.find("inputmethod") != std::string::npos) {
        if (!cfg.process_keyboard) return false;
    }

    return true;
}

Config get_effective(const std::string& package_name) {
    Config result = s_config;
    auto it = s_config.profiles.find(package_name);
    if (it == s_config.profiles.end()) return result;

    const AppProfile& p = it->second;
    if (p.mode >= 0) result.mode = static_cast<RenderMode>(p.mode);
    if (p.quality >= 0) result.quality = static_cast<ShaderQuality>(p.quality);
    if (p.edge_threshold >= 0) result.edge_threshold = p.edge_threshold;
    if (p.smooth_min >= 0) result.smooth_min = p.smooth_min;
    if (p.smooth_max >= 0) result.smooth_max = p.smooth_max;
    if (p.line_color[0] >= 0) result.line_color[0] = p.line_color[0];
    if (p.line_color[1] >= 0) result.line_color[1] = p.line_color[1];
    if (p.line_color[2] >= 0) result.line_color[2] = p.line_color[2];
    if (p.bg_color[0] >= 0) result.bg_color[0] = p.bg_color[0];
    if (p.bg_color[1] >= 0) result.bg_color[1] = p.bg_color[1];
    if (p.bg_color[2] >= 0) result.bg_color[2] = p.bg_color[2];
    if (p.bg_opacity >= 0) result.bg_opacity = p.bg_opacity;
    if (p.line_intensity >= 0) result.line_intensity = p.line_intensity;
    if (p.resolution_scale >= 0) result.resolution_scale = p.resolution_scale;
    if (p.frame_skip >= 0) result.frame_skip = p.frame_skip;
    if (p.use_half_float_set) result.use_half_float = p.use_half_float;
    if (p.use_compute_set) result.use_compute = p.use_compute;

    LOGI("Applied profile for %s: mode=%d quality=%d threshold=%.2f intensity=%.1f",
         package_name.c_str(), (int)result.mode, (int)result.quality,
         result.edge_threshold, result.line_intensity);

    return result;
}

void set_package(const std::string& package_name) {
    s_active_package = package_name;
}

const std::string& get_package() {
    return s_active_package;
}

bool was_updated() {
    return s_updated.exchange(false);
}

} // namespace config
