#pragma once
#include <stdint.h>

typedef struct {
    const char *name; // Nombre Mach-O (con '_' inicial si aplica)
    uintptr_t addr;
    int data;         // 1 = variable/datos (enlace directo); 0 = función (entra por ml_align_call)
} import_entry;

// Busca la implementación de un símbolo importado por el juego, o NULL si no existe.
const import_entry *imports_lookup(const char *name);

// Tablas modulares por subsistema
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

// Convención softfp de iOS ARM: floats y doubles viajan en registros enteros (r0..r3).
// Como la PS Vita compila con hardfp por defecto en vitasdk, todo import de función
// que reciba o retorne float debe decorarse con este atributo.
#define SOFTFP __attribute__((pcs("aapcs")))

#define F(name, fn)    {"_" #name, (uintptr_t)(fn), 0}
#define F_RAW(name, fn){name, (uintptr_t)(fn), 0}
#define D(name, addr)  {"_" #name, (uintptr_t)(addr), 1}
