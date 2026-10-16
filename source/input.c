// Manejo de Input para PS Vita -> iOS.
// Captura multitouch frontal y botones fisicos / sticks analogicos.
// Provee mapeo de coordenadas al espacio virtual del juego.
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <string.h>

#include "input.h"
#include "log.h"
#include "macho.h"

touch_point touches[SCE_TOUCH_MAX_REPORT];
int touch_count = 0;
SceCtrlData pad;

static touch_point prev_touches[SCE_TOUCH_MAX_REPORT];
static int prev_count = 0;

void input_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    memset(touches, 0, sizeof(touches));
    memset(prev_touches, 0, sizeof(prev_touches));
    touch_count = 0;
    prev_count = 0;
    l_info("Subsistema de input inicializado (touch frontal + analogicos).");
}

void input_poll(void) {
    sceCtrlPeekBufferPositive(0, &pad, 1);

    SceTouchData t;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 1)
        t.reportNum = 0;

    memcpy(prev_touches, touches, sizeof(touches));
    prev_count = touch_count;

    touch_count = (int)t.reportNum;
    if (touch_count > SCE_TOUCH_MAX_REPORT)
        touch_count = SCE_TOUCH_MAX_REPORT;

    for (int i = 0; i < touch_count; i++) {
        touches[i].id = t.report[i].id;
        touches[i].x = t.report[i].x / 2;
        touches[i].y = t.report[i].y / 2;
        // Mapeo basico a 480x320 apaisado centrado en la pantalla 960x544
        // (el juego o patch.c puede sobreescribir este mapeo segun su resolucion)
        int gx = (touches[i].x - 72) * 480 / 816;
        int gy = touches[i].y * 320 / 544;
        touches[i].gx = gx < 0 ? 0 : gx > 479 ? 479 : gx;
        touches[i].gy = gy < 0 ? 0 : gy > 319 ? 319 : gy;
        touches[i].down = 1;
    }
}
