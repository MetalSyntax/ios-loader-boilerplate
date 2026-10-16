#pragma once
#include <stdint.h>

void gles_init(void);
// Presenta el frame: FBO vertical de iOS -> pantalla apaisada de la Vita.
void gles_present(void);
uint32_t gles_get_draw_calls(void);
