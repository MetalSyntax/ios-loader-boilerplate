/**
 * @file vitagl_no_splash.c
 * @brief Overrides vitaGL default splashscreen routines to eliminate frame stalls.
 */
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>

uint8_t is_splashscreen_active = 0;
SceUID splash_mutex[2] = {-1, -1};

void invoke_splashscreen(void) {
}

void clear_splashscreen(void) {
}
