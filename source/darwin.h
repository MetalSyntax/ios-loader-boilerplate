#pragma once
#include <stddef.h>

void darwin_init(void);
const char *darwin_path(const char *in, char *out, size_t outlen);
