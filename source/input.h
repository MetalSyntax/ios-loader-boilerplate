#pragma once
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <stdint.h>

typedef struct {
    int id;
    int x; // Coordenadas en espacio de pantalla de la Vita (0..960, 0..544)
    int y;
    int gx; // Coordenadas proyectadas al espacio del juego (por defecto 480x320 o configurable)
    int gy;
    int down;
} touch_point;

extern touch_point touches[SCE_TOUCH_MAX_REPORT];
extern int touch_count;
extern SceCtrlData pad;

void input_init(void);
void input_poll(void);
