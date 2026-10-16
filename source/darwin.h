/**
 * @file darwin.h
 * @brief Darwin POSIX/libc/pthreads subsystem bridge for PS Vita.
 */
#pragma once
#include <stddef.h>

void darwin_init(void);
const char *darwin_path(const char *in, char *out, size_t outlen);
