#pragma once
#include <stdint.h>

// El ejecutable iOS está enlazado en direcciones fijas (típicamente 0x1000).
// Se carga desplazado a una región libre de la Vita: la dirección en tiempo de ejecución
// es la dirección original del Mach-O + MACHO_SLIDE.
// Así, para traducir un PC de un crash dump se resta simplemente 0x98000000.
#define MACHO_SLIDE 0x98000000u

int macho_load(const char *bin_path, const char *rebase_path);
void macho_run_inits(void);

// Dirección real de un símbolo del juego (nombre Mach-O, con '_' inicial si lo tiene);
// incluye el bit Thumb si aplica. Devuelve 0 si no existe.
uintptr_t macho_sym(const char *name);

// Redirige una función del juego a una implementación propia mediante un hook directo.
void macho_hook(const char *name, void *dst);

// Llama a una función del juego preservando r9 (el código iOS lo usa como temporal).
uint32_t call_guest(void *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
uint32_t call_guest6(void *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
