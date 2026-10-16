/**
 * @file imports.h
 * @brief Dynamic import table definitions and SoftFP calling conventions.
 */
#pragma once
#include <stdint.h>

/**
 * @brief Represents a single imported symbol binding.
 */
typedef struct {
    const char *name; /**< Mach-O symbol name (including leading '_' if present). */
    uintptr_t addr;   /**< Destination function address or data pointer. */
    int data;         /**< 1 = variable/data (direct link); 0 = function (dispatched via ml_align_call). */
} import_entry;

/**
 * @brief Resolves an imported symbol by Mach-O name across all registered tables.
 * @param[in] name Symbol name string to locate.
 * @return Pointer to matching import_entry, or NULL if unhandled.
 */
const import_entry *imports_lookup(const char *name);

// Modular subsystem symbol tables
extern const import_entry darwin_imports[];
extern const int darwin_imports_count;

extern const import_entry gles_imports[];
extern const int gles_imports_count;

extern const import_entry coreaudio_imports[];
extern const int coreaudio_imports_count;

extern const import_entry objc_imports[];
extern const int objc_imports_count;

extern const import_entry patch_imports[];
extern const int patch_imports_count;

// iOS ARM SoftFP calling convention: float and double arguments pass in integer registers (r0..r3).
// Because VitaSDK targets hardfp by default, any imported function that passes or returns floats
// must be decorated with this attribute.
#define SOFTFP __attribute__((pcs("aapcs")))

#define F(name, fn)    {"_" #name, (uintptr_t)(fn), 0}
#define F_RAW(name, fn){name, (uintptr_t)(fn), 0}
#define D(name, addr)  {"_" #name, (uintptr_t)(addr), 1}
