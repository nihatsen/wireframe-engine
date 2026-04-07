#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// ═══════════════════════════════════════════════════════════════
//  PLT/GOT HOOKING ENGINE
//  Lightweight ELF-based function interception for ARM32/ARM64
// ═══════════════════════════════════════════════════════════════

namespace hook {

// Hook a function by name across all loaded libraries
// Returns 0 on success
int hook_plt(
    const char* symbol_name,
    void* replacement,
    void** original_out
);

// Hook in a specific library only
int hook_plt_in_library(
    const char* library_name,
    const char* symbol_name,
    void* replacement,
    void** original_out
);

// Unhook a previously hooked function
int unhook_plt(const char* symbol_name, void* original);

// Re-scan all loaded libraries and hook any new ones that appeared since last scan
// Returns number of newly hooked libraries
int rehook_new_libraries(const char* symbol_name, void* replacement);

// Inline hook: patch the first instructions of a function to jump to replacement.
// Saves the original bytes so they can be executed via a trampoline.
// Works even when the caller used dlsym() to get the function pointer.
// Returns 0 on success. *trampoline_out receives a callable pointer to the original.
int inline_hook(void* target, void* replacement, void** trampoline_out);

// Get the base address of a loaded library
uintptr_t get_module_base(const char* module_name);

// Internal: ELF parsing helpers
namespace elf {
    struct ModuleInfo {
        std::string name;
        uintptr_t base;
        uintptr_t end;
    };

    std::vector<ModuleInfo> get_loaded_modules();
    int patch_got_entry(uintptr_t base, const char* symbol, void* replacement, void** original);
}

} // namespace hook
