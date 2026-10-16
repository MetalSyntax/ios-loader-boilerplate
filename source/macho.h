/**
 * @file macho.h
 * @brief 32-bit Mach-O executable loader and runtime relocator for PS Vita.
 */
#pragma once
#include <stdint.h>

/**
 * @brief Fixed virtual address offset where 32-bit iOS Mach-O binaries are loaded on PS Vita.
 * @details The original iOS executable is typically linked at base 0x1000.
 *          It is loaded shifted to an unused Vita user memory space:
 *          runtime_addr = original_addr + MACHO_SLIDE.
 *          Crash PC addresses can be translated directly by subtracting 0x98000000.
 */
#define MACHO_SLIDE 0x98000000u

/**
 * @brief Loads a Mach-O executable, allocates memory blocks, and applies rebases.
 * @param[in] bin_path Path to Mach-O binary (thin or fat ARMv6/v7).
 * @param[in] rebase_path Path to offline rebase relocation table (rebase.bin).
 * @return 0 on success, negative error code on failure.
 */
int macho_load(const char *bin_path, const char *rebase_path);

/**
 * @brief Executes static C/C++ global constructors (__mod_init_func section).
 */
void macho_run_inits(void);

/**
 * @brief Resolves the runtime memory address of a guest Mach-O symbol.
 * @param[in] name Mach-O symbol name (including leading '_' if present).
 * @return Runtime address (with Thumb bit 0 set if applicable), or 0 if not found.
 */
uintptr_t macho_sym(const char *name);

/**
 * @brief Overwrites a guest function entry point with an unconditional trampoline hook.
 * @param[in] name Target guest symbol name.
 * @param[in] dst Destination hook function pointer.
 */
void macho_hook(const char *name, void *dst);

/**
 * @brief Invokes a guest function preserving r9 register.
 */
uint32_t call_guest(void *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

/**
 * @brief Invokes a 6-argument guest function preserving r9 register.
 */
uint32_t call_guest6(void *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
