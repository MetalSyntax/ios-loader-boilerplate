#pragma once
#include "imports.h"

// Inicializa hooks y parches en memoria especificos del juego
void patch_init(void);

// Callback por frame si el motor requiere logica adicional
void patch_frame(void);
