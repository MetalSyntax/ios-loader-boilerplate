#pragma once
#include <stddef.h>
#include <stdint.h>

// 1 si data es un PNG CgBI de iOS.
int cgbi_is(const void *data, int len);

// CgBI -> RGBA8 premultiplicado (como lo deja CoreGraphics), fila superior primero, en memoria de
// alloc. bpp = bits por píxel del original (24/32). NULL si el formato no es el esperado.
uint8_t *cgbi_decode(const void *data, int len, int *w, int *h, int *bpp, void *(*alloc)(size_t));
