/**
 * @file darwin_printf.c
 * @brief Custom printf engine for the iOS ARMv6 variadic ABI.
 * @details In iOS ARMv6, variadic arguments are packed into 32-bit words WITHOUT 8-byte alignment:
 *          a double or long long occupies the next 2 consecutive words (even across r3 and stack).
 *          Standard newlib (AAPCS) forces 8-byte alignment and reads garbage. This implementation
 *          consumes arguments word-by-word, supporting %@ (NSString/CFString with C-string at +8)
 *          and unaligned double/long long arguments.
 *          Self-contained with no PS Vita dependencies (verified via tools/printf_check.py).
 */
#include <stdio.h>
#include <string.h>

#include "darwin_printf.h"

// In iOS ARMv6, variadic arguments are packed into 32-bit words without 8-byte alignment:
// a double or long long occupies the next 2 words. newlib (AAPCS) would align to 8 and read
// garbage, so we iterate through the format consuming word by word.
// p != NULL: Darwin va_list (pointer to words); otherwise variadic arguments of this call.
static uint32_t word(words *w) { return w->p ? *w->p++ : va_arg(*w->ap, uint32_t); }
static uint64_t dword(words *w) {
    uint32_t lo = word(w);
    return (uint64_t)word(w) << 32 | lo;
}

int darwin_vformat(char *out, size_t size, const char *fmt, words *w) {
    size_t n = 0; // total length, snprintf semantics (even if truncated)
    char spec[40];
    while (*fmt) {
        if (*fmt != '%') {
            if (n + 1 < size)
                out[n] = *fmt;
            n++, fmt++;
            continue;
        }
        size_t k = 0;
        int star[2], nstar = 0, ll = 0;
        spec[k++] = *fmt++;
        while (*fmt && strchr("-+ #0'", *fmt) && k < 8)
            spec[k++] = *fmt++;
        for (int part = 0; part < 2; part++) { // width and precision
            if (part && *fmt == '.')
                spec[k++] = *fmt++;
            else if (part)
                break;
            if (*fmt == '*') {
                star[nstar++] = word(w);
                spec[k++] = *fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9' && k < 24)
                spec[k++] = *fmt++;
        }
        while (*fmt && strchr("hlLqjzt", *fmt)) {
            if (*fmt == 'q' || *fmt == 'j' || (fmt[0] == 'l' && fmt[1] == 'l'))
                ll = 1;
            fmt += (fmt[0] == 'l' && fmt[1] == 'l') ? 2 : 1;
        }
        char conv = *fmt ? *fmt++ : 0;
        char *dst = n < size ? out + n : NULL;
        size_t room = n < size ? size - n : 0;
#define EMIT(v) (nstar == 0 ? snprintf(dst, room, spec, v) \
                 : nstar == 1 ? snprintf(dst, room, spec, star[0], v) : snprintf(dst, room, spec, star[0], star[1], v))
        int r = 0;
        if (ll && strchr("diouxX", conv)) {
            spec[k++] = 'l', spec[k++] = 'l', spec[k++] = conv, spec[k] = 0;
            r = EMIT(dword(w));
        } else if (conv && strchr("diouxXc", conv)) {
            spec[k++] = conv, spec[k] = 0;
            r = EMIT(word(w));
        } else if (conv && strchr("fFeEgGaA", conv)) {
            union { uint64_t u; double d; } v = {dword(w)};
            spec[k++] = conv, spec[k] = 0;
            r = EMIT(v.d);
        } else if (conv == 's' || conv == '@') { // %@: NSString/CFString, C string pointer at offset +8
            uint32_t v = word(w);
            const char *s = !v ? "(null)" : conv == '@' ? *(const char **)(uintptr_t)(v + 8) : (const char *)(uintptr_t)v;
            spec[k++] = 's', spec[k] = 0;
            r = EMIT(s);
        } else if (conv == 'p') {
            spec[k++] = 'p', spec[k] = 0;
            r = EMIT((void *)(uintptr_t)word(w));
        } else if (conv == 'n') {
            word(w);
        } else if (conv) { // %% and unknown conversion specifiers: emit literal
            r = snprintf(dst, room, "%c", conv);
        }
#undef EMIT
        n += r > 0 ? r : 0;
    }
    if (size)
        out[n < size ? n : size - 1] = 0;
    return n;
}
