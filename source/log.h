#pragma once

void log_open(const char *path);
void l_info(const char *fmt, ...);
void l_warn(const char *fmt, ...);
void l_error(const char *fmt, ...);
void fatal_error(const char *fmt, ...) __attribute__((noreturn));
