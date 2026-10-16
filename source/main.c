#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <stdio.h>
#include <string.h>

#include "darwin.h"
#include "gles.h"
#include "hle.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "objc.h"
#include "patch.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;
// El hilo principal de iOS requiere buen margen de stack (4 MB)
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

// Sistema de logs rotativo (log_001.log .. log_999.log)
static int next_log_number(void) {
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(LOGS_PATH, 0777);
    int last = 0, n;
    uint64_t newest = 0;
    SceUID d = sceIoDopen(LOGS_PATH);
    SceIoDirent e;
    while (d >= 0 && sceIoDread(d, &e) > 0) {
        const SceDateTime *t = &e.d_stat.st_mtime;
        uint64_t key = (uint64_t)t->year << 48 | (uint64_t)t->month << 40 | (uint64_t)t->day << 32 |
                       (uint64_t)t->hour << 24 | t->minute << 16 | t->second << 8 | t->microsecond / 10000;
        if (sscanf(e.d_name, "log_%3d.log", &n) == 1 && n >= 1 && n <= 999 && key >= newest) {
            newest = key;
            last = n;
        }
    }
    if (d >= 0)
        sceIoDclose(d);
    return (last % 999) + 1;
}

int main(int argc, char *argv[]) {
    // Overclock estándar de PS Vita para emulación fluida
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Preparar directorio de guardado y logs
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(DOCS_PATH, 0777);
    char log_path[128];
    snprintf(log_path, sizeof(log_path), LOGS_PATH "log_%03d.log", next_log_number());
    log_open(log_path);
    l_info("=== iOS Mach-O Loader para PS Vita Iniciando ===");

    // Comprobación de runtime ShaRKBR33D
    if (sceIoGetstat("ur0:/data/libshacccg.suprx", &(SceIoStat){0}) < 0 &&
        sceIoGetstat("ur0:/data/external/libshacccg.suprx", &(SceIoStat){0}) < 0) {
        fatal_error("Falta libshacccg.suprx (instalarlo con ShaRKBR33D en la consola).");
    }

    // Inicializar AppUtil y diálogos comunes
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);

    SceCommonDialogConfigParam dlg;
    sceCommonDialogConfigParamInit(&dlg);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&dlg.language);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&dlg.enterButtonAssign);
    sceCommonDialogSetConfigParam(&dlg);

    // Inicializar subsistemas
    gles_init();
    darwin_init();
    input_init();

    // Cargar ejecutable Mach-O del juego
    // Nota: El ejecutable se busca dentro del bundle game_app/
    // o el nombre configurado en patch.c / CMakeLists
    l_info("Cargando ejecutable principal Mach-O...");
    int res = macho_load(BUNDLE_PATH "game_exec", DATA_PATH "rebase.bin");
    if (res < 0) {
        // Fallback: intentar leer binarios comunes si 'game_exec' fue renombrado
        fatal_error("No se pudo cargar el ejecutable Mach-O en %s", BUNDLE_PATH "game_exec");
    }

    // Inicializar parches y hooks
    patch_init();

    // Ejecutar constructores estáticos C/C++ (__mod_init_func)
    macho_run_inits();

    // Buscar punto de entrada
    uintptr_t entry = macho_sym("_main");
    if (!entry)
        entry = macho_sym("_SDL_main");
    if (!entry)
        entry = macho_sym("_startClet");

    if (entry) {
        l_info("Saltando al punto de entrada en 0x%08X (offset Mach-O 0x%08X)",
               (unsigned)entry, (unsigned)(entry - MACHO_SLIDE));
        call_guest((void *)entry, 0, 0, 0, 0);
    } else {
        l_warn("No se encontro '_main' ni '_startClet'; entrando en bucle de eventos...");
    }

    // Bucle principal por defecto si el juego opera por callbacks o timers
    for (;;) {
        input_poll();
        ime_poll();
        patch_frame();
        gles_present();
        sceKernelDelayThread(16000); // ~60 fps
    }

    return 0;
}
