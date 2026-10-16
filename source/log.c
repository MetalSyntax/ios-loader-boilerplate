/**
 * @file log.c
 * @brief Thread-safe logging subsystem implementation.
 */
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static FILE *log_file = NULL;

void log_open(const char *path) {
    if (log_file)
        fclose(log_file);
    log_file = fopen(path, "w");
    if (!log_file)
        printf("[-] Failed to open log file at %s\n", path);
}

static void log_v(const char *level, const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d.%03d",
             t.hour, t.minute, t.second, t.microsecond / 1000);

    printf("[%s] [%s] %s\n", time_str, level, buf);
    if (log_file) {
        fprintf(log_file, "[%s] [%s] %s\n", time_str, level, buf);
        fflush(log_file);
    }
}

void l_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v("INFO", fmt, ap);
    va_end(ap);
}

void l_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v("WARN", fmt, ap);
    va_end(ap);
}

void l_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v("ERROR", fmt, ap);
    va_end(ap);
}

void fatal_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v("FATAL", fmt, ap);
    va_end(ap);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    sceKernelExitProcess(1);
    while (1) {}
}
