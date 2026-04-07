#include "hook.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

// Safely check if a memory region is readable without risking SIGSEGV.
// Uses pipe write: kernel returns EFAULT instead of delivering a signal.
static bool is_readable(const void* addr, size_t len) {
    int fd[2];
    if (pipe(fd) < 0) return false;
    bool readable = (write(fd[1], addr, len) == static_cast<ssize_t>(len));
    close(fd[0]);
    close(fd[1]);
    return readable;
}

#define LOG_TAG "WireframeHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef __LP64__
    using ElfW_Ehdr = Elf64_Ehdr;
    using ElfW_Phdr = Elf64_Phdr;
    using ElfW_Shdr = Elf64_Shdr;
    using ElfW_Dyn  = Elf64_Dyn;
    using ElfW_Sym  = Elf64_Sym;
    using ElfW_Rel  = Elf64_Rela;
    #define ELF_R_SYM(x) ELF64_R_SYM(x)
    #define ELF_R_TYPE(x) ELF64_R_TYPE(x)
    #define RELOC_TYPE_PLT R_AARCH64_JUMP_SLOT
    #define RELOC_TYPE_GLOB R_AARCH64_GLOB_DAT
#else
    using ElfW_Ehdr = Elf32_Ehdr;
    using ElfW_Phdr = Elf32_Phdr;
    using ElfW_Shdr = Elf32_Shdr;
    using ElfW_Dyn  = Elf32_Dyn;
    using ElfW_Sym  = Elf32_Sym;
    using ElfW_Rel  = Elf32_Rel;
    #define ELF_R_SYM(x) ELF32_R_SYM(x)
    #define ELF_R_TYPE(x) ELF32_R_TYPE(x)
    #define RELOC_TYPE_PLT R_ARM_JUMP_SLOT
    #define RELOC_TYPE_GLOB R_ARM_GLOB_DAT
#endif

namespace hook {

static std::mutex s_hook_mutex;
static std::set<std::string> s_hooked_modules;

namespace elf {

std::vector<ModuleInfo> get_loaded_modules() {
    std::vector<ModuleInfo> modules;
    std::ifstream maps("/proc/self/maps");
    std::string line;

    while (std::getline(maps, line)) {
        if (line.find("r-xp") != std::string::npos ||
            line.find("r--p") != std::string::npos) {

            uintptr_t start, end;
            char perms[5];
            unsigned long offset;
            int dev_major, dev_minor;
            unsigned long inode;
            char path[512] = {0};

            if (sscanf(line.c_str(), "%lx-%lx %4s %lx %x:%x %lu %511s",
                        &start, &end, perms, &offset, &dev_major,
                        &dev_minor, &inode, path) >= 7) {

                if (path[0] == '/' && offset == 0) {
                    ModuleInfo info;
                    info.name = path;
                    info.base = start;
                    info.end = end;

                    // Check for duplicate (we want the first mapping)
                    bool found = false;
                    for (auto& m : modules) {
                        if (m.name == info.name) {
                            if (info.end > m.end) m.end = info.end;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        modules.push_back(info);
                    }
                }
            }
        }
    }
    return modules;
}

int patch_got_entry(uintptr_t base, const char* symbol, void* replacement, void** original) {
    // Validate memory is still accessible before dereferencing
    // (library may have been unloaded between /proc/self/maps scan and now)
    if (!is_readable(reinterpret_cast<const void*>(base), sizeof(ElfW_Ehdr))) {
        return -1;
    }

    auto* ehdr = reinterpret_cast<ElfW_Ehdr*>(base);

    // Validate ELF magic
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        return -1;
    }

    auto* phdr = reinterpret_cast<ElfW_Phdr*>(base + ehdr->e_phoff);

    // Find PT_DYNAMIC segment
    ElfW_Dyn* dynamic = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dynamic = reinterpret_cast<ElfW_Dyn*>(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dynamic) return -2;
    if (!is_readable(reinterpret_cast<const void*>(dynamic), sizeof(ElfW_Dyn))) {
        return -2;
    }

    // Parse dynamic section
    ElfW_Sym* dynsym = nullptr;
    const char* dynstr = nullptr;
    ElfW_Rel* relplt = nullptr;
    ElfW_Rel* reldyn = nullptr;
    size_t relplt_size = 0;
    size_t reldyn_size = 0;

    for (ElfW_Dyn* d = dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:
                dynsym = reinterpret_cast<ElfW_Sym*>(base + d->d_un.d_ptr);
                break;
            case DT_STRTAB:
                dynstr = reinterpret_cast<const char*>(base + d->d_un.d_ptr);
                break;
            case DT_JMPREL:
                relplt = reinterpret_cast<ElfW_Rel*>(base + d->d_un.d_ptr);
                break;
            case DT_PLTRELSZ:
                relplt_size = d->d_un.d_val;
                break;
#ifdef __LP64__
            case DT_RELA:
                reldyn = reinterpret_cast<ElfW_Rel*>(base + d->d_un.d_ptr);
                break;
            case DT_RELASZ:
                reldyn_size = d->d_un.d_val;
                break;
#else
            case DT_REL:
                reldyn = reinterpret_cast<ElfW_Rel*>(base + d->d_un.d_ptr);
                break;
            case DT_RELSZ:
                reldyn_size = d->d_un.d_val;
                break;
#endif
        }
    }

    if (!dynsym || !dynstr) return -3;

    auto try_patch = [&](ElfW_Rel* rel, size_t size) -> int {
        if (!rel || size == 0) return -1;

        size_t count = size / sizeof(ElfW_Rel);
        for (size_t i = 0; i < count; i++) {
            uint32_t type = ELF_R_TYPE(rel[i].r_info);
            if (type != RELOC_TYPE_PLT && type != RELOC_TYPE_GLOB) continue;

            uint32_t sym_idx = ELF_R_SYM(rel[i].r_info);
            const char* sym_name = dynstr + dynsym[sym_idx].st_name;

            if (strcmp(sym_name, symbol) == 0) {
                uintptr_t* got_entry = reinterpret_cast<uintptr_t*>(
                    base + rel[i].r_offset);

                // Make GOT entry writable
                uintptr_t page_start = reinterpret_cast<uintptr_t>(got_entry) & ~(getpagesize() - 1);
                size_t page_size = getpagesize();

                if (mprotect(reinterpret_cast<void*>(page_start),
                            page_size, PROT_READ | PROT_WRITE) != 0) {
                    LOGE("mprotect failed for GOT entry");
                    return -4;
                }

                // Save original and patch
                if (original) {
                    *original = reinterpret_cast<void*>(*got_entry);
                }
                *got_entry = reinterpret_cast<uintptr_t>(replacement);

                // Restore protection
                mprotect(reinterpret_cast<void*>(page_start),
                         page_size, PROT_READ);

                // Clear instruction cache
                __builtin___clear_cache(
                    reinterpret_cast<char*>(got_entry),
                    reinterpret_cast<char*>(got_entry) + sizeof(uintptr_t));

                return 0;
            }
        }
        return -1;
    };

    // Try PLT relocations first, then dynamic relocations
    int result = try_patch(relplt, relplt_size);
    if (result != 0) {
        result = try_patch(reldyn, reldyn_size);
    }

    return result;
}

} // namespace elf

int hook_plt(const char* symbol_name, void* replacement, void** original_out) {
    std::lock_guard<std::mutex> lock(s_hook_mutex);

    auto modules = elf::get_loaded_modules();
    bool hooked_any = false;

    for (auto& mod : modules) {
        // Skip our own library and linker
        if (mod.name.find("wireframe") != std::string::npos) continue;
        if (mod.name.find("linker") != std::string::npos) continue;

        int result = elf::patch_got_entry(mod.base, symbol_name, replacement,
                                           hooked_any ? nullptr : original_out);
        if (result == 0) {
            LOGI("Hooked %s in %s", symbol_name, mod.name.c_str());
            hooked_any = true;
        }
        // Track all modules we've seen during initial hook
        s_hooked_modules.insert(mod.name);
    }

    return hooked_any ? 0 : -1;
}

int hook_plt_in_library(const char* library_name, const char* symbol_name,
                         void* replacement, void** original_out) {
    std::lock_guard<std::mutex> lock(s_hook_mutex);

    auto modules = elf::get_loaded_modules();
    for (auto& mod : modules) {
        if (mod.name.find(library_name) != std::string::npos) {
            return elf::patch_got_entry(mod.base, symbol_name, replacement, original_out);
        }
    }
    return -1;
}

int unhook_plt(const char* symbol_name, void* original) {
    return hook_plt(symbol_name, original, nullptr);
}

int rehook_new_libraries(const char* symbol_name, void* replacement) {
    std::lock_guard<std::mutex> lock(s_hook_mutex);

    auto modules = elf::get_loaded_modules();
    int new_hooks = 0;

    for (auto& mod : modules) {
        // Skip already-hooked, our own library, and linker
        if (s_hooked_modules.count(mod.name)) continue;
        if (mod.name.find("wireframe") != std::string::npos) continue;
        if (mod.name.find("linker") != std::string::npos) continue;

        int result = elf::patch_got_entry(mod.base, symbol_name, replacement, nullptr);
        if (result == 0) {
            LOGI("Late-hooked %s in %s", symbol_name, mod.name.c_str());
            new_hooks++;
        }
        // Mark as processed regardless of result to avoid retrying
        s_hooked_modules.insert(mod.name);
    }

    return new_hooks;
}

// ─── Inline Hook (ARM64) ────────────────────────────────────
// Patches the entry point of a function with a trampoline jump.
// This catches ALL callers including those that resolved via dlsym().
#ifdef __LP64__
int inline_hook(void* target, void* replacement, void** trampoline_out) {
    if (!target || !replacement) return -1;

    uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);

    // ARM64 inline hook: overwrite first 16 bytes with:
    //   LDR X16, [PC, #8]   ; load target address from next 8 bytes
    //   BR  X16              ; jump to it
    //   <8-byte address>     ; replacement function address
    // Total: 16 bytes (4 instructions worth)

    const size_t hook_size = 16;
    const size_t trampoline_size = hook_size + 16; // saved bytes + jump back

    // Allocate executable trampoline
    void* trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) {
        LOGE("inline_hook: mmap failed for trampoline");
        return -2;
    }

    auto* tramp = reinterpret_cast<uint8_t*>(trampoline);

    // Step 1: Copy original bytes to trampoline
    if (!is_readable(target, hook_size)) {
        munmap(trampoline, 4096);
        return -3;
    }
    memcpy(tramp, target, hook_size);

    // Step 2: Append jump-back to trampoline (jump to target + hook_size)
    // LDR X16, [PC, #8]
    // BR X16
    // <address of target + hook_size>
    uintptr_t resume_addr = target_addr + hook_size;
    uint32_t ldr_x16 = 0x58000050; // LDR X16, [PC, #8]
    uint32_t br_x16  = 0xD61F0200; // BR X16
    memcpy(tramp + hook_size, &ldr_x16, 4);
    memcpy(tramp + hook_size + 4, &br_x16, 4);
    memcpy(tramp + hook_size + 8, &resume_addr, 8);

    // Flush instruction cache for trampoline
    __builtin___clear_cache(reinterpret_cast<char*>(tramp),
                            reinterpret_cast<char*>(tramp + trampoline_size));

    // Step 3: Make target writable and patch it
    uintptr_t page_start = target_addr & ~(getpagesize() - 1);
    uintptr_t page_end = (target_addr + hook_size + getpagesize() - 1) & ~(getpagesize() - 1);
    size_t prot_size = page_end - page_start;

    if (mprotect(reinterpret_cast<void*>(page_start), prot_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("inline_hook: mprotect failed: %s", strerror(errno));
        munmap(trampoline, 4096);
        return -4;
    }

    // Write hook: LDR X16, [PC, #8]; BR X16; <replacement addr>
    uintptr_t repl_addr = reinterpret_cast<uintptr_t>(replacement);
    auto* patch = reinterpret_cast<uint8_t*>(target);
    memcpy(patch, &ldr_x16, 4);
    memcpy(patch + 4, &br_x16, 4);
    memcpy(patch + 8, &repl_addr, 8);

    // Restore protection
    mprotect(reinterpret_cast<void*>(page_start), prot_size, PROT_READ | PROT_EXEC);

    // Flush instruction cache
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(patch + hook_size));

    if (trampoline_out) {
        *trampoline_out = trampoline;
    }

    LOGI("Inline hook installed at %p -> %p (trampoline at %p)", target, replacement, trampoline);
    return 0;
}
#else
// ARM32 inline hook
int inline_hook(void* target, void* replacement, void** trampoline_out) {
    if (!target || !replacement) return -1;

    uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
    // Handle Thumb mode (bit 0 set)
    bool is_thumb = target_addr & 1;
    target_addr &= ~1u;

    const size_t hook_size = 8; // LDR PC, [PC, #0]; <addr>

    void* trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return -2;

    auto* tramp = reinterpret_cast<uint8_t*>(trampoline);

    if (!is_readable(reinterpret_cast<void*>(target_addr), hook_size)) {
        munmap(trampoline, 4096);
        return -3;
    }
    memcpy(tramp, reinterpret_cast<void*>(target_addr), hook_size);

    // Jump back: LDR PC, [PC, #0]; <resume_addr>
    uintptr_t resume_addr = target_addr + hook_size + (is_thumb ? 1 : 0);
    uint32_t ldr_pc = 0xE51FF004; // LDR PC, [PC, #-4]
    memcpy(tramp + hook_size, &ldr_pc, 4);
    memcpy(tramp + hook_size + 4, &resume_addr, 4);

    __builtin___clear_cache(reinterpret_cast<char*>(tramp),
                            reinterpret_cast<char*>(tramp + hook_size + 8));

    uintptr_t page_start = target_addr & ~(getpagesize() - 1);
    size_t prot_size = getpagesize() * 2;
    if (mprotect(reinterpret_cast<void*>(page_start), prot_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        munmap(trampoline, 4096);
        return -4;
    }

    uintptr_t repl_addr = reinterpret_cast<uintptr_t>(replacement);
    auto* patch = reinterpret_cast<uint8_t*>(target_addr);
    memcpy(patch, &ldr_pc, 4);
    memcpy(patch + 4, &repl_addr, 4);

    mprotect(reinterpret_cast<void*>(page_start), prot_size, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char*>(target_addr),
                            reinterpret_cast<char*>(target_addr + hook_size));

    if (trampoline_out) {
        *trampoline_out = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(trampoline) + (is_thumb ? 1 : 0));
    }

    LOGI("Inline hook (ARM32) installed at %p -> %p", target, replacement);
    return 0;
}
#endif

uintptr_t get_module_base(const char* module_name) {
    auto modules = elf::get_loaded_modules();
    for (auto& mod : modules) {
        if (mod.name.find(module_name) != std::string::npos) {
            return mod.base;
        }
    }
    return 0;
}

} // namespace hook
