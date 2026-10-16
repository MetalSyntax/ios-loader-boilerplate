/**
 * @file darwin_printf.h
 * @brief Darwin ARM variadic string formatter with %@ and unaligned 64-bit argument support.
 */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Variadic argument state for guest calls.
 * @details Under the iOS ARM ABI, variadic arguments are a packed sequence of unaligned
 *          32-bit words. When p != NULL, it points to a Darwin va_list; otherwise ap is used.
 */
typedef struct {
    va_list *ap;
    const uint32_t *p;
} words;

/**
 * @brief Formats a Darwin-compatible variadic string into a buffer.
 * @param[out] out Destination character buffer.
 * @param[in] size Maximum buffer capacity in bytes.
 * @param[in] fmt Format string (supports %@ for NSString* and unaligned 64-bit words).
 * @param[in,out] w Variadic argument stream iterator.
 * @return Total number of characters that would have been generated, excluding null terminator.
 */
int darwin_vformat(char *out, size_t size, const char *fmt, words *w);
