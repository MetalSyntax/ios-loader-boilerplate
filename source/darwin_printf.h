#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// Argumentos variádicos de una llamada desde el juego (ABI de iOS: palabras de 32 bits sin
// alinear). p != NULL: va_list de Darwin (puntero a las palabras, para v*printf); si no, ap.
typedef struct {
    va_list *ap;
    const uint32_t *p;
} words;

// Como vsnprintf, con %@ (NSString/CFString: cadena C en +8) y doubles/long long en 2 palabras.
int darwin_vformat(char *out, size_t size, const char *fmt, words *w);
