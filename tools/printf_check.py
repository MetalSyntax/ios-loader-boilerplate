#!/usr/bin/env python3
"""Verifica source/darwin_printf.c en el host: argumentos variádicos con el ABI de iOS ARMv6
(palabras de 32 bits sin alinear; double/long long en 2 palabras seguidas).
Uso: python3 -I tools/printf_check.py
"""
import os, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = r'''
#include <stdio.h>
#include <string.h>
#include "darwin_printf.h"
static int check(const char *fmt, const uint32_t *args, size_t size, const char *want, int want_len) {
    char out[128];
    words w = {NULL, args};
    int n = darwin_vformat(out, size, fmt, &w);
    if (n != want_len || strcmp(out, want)) { printf("FALLA %s -> \"%s\" (%d), esperado \"%s\" (%d)\n", fmt, out, n, want, want_len); return 1; }
    return 0;
}
int main(void) {
    union { double d; uint32_t w[2]; } pi = {3.25};
    long long neg = -5;
    uint32_t a[] = {7, pi.w[0], pi.w[1], (uint32_t)neg, (uint32_t)(neg >> 32), 0xBEEF, 4, 42};
    int bad = check("%d %.2f %lld %x %*d %%", a, 128, "7 3.25 -5 beef   42 %", 21);
    bad += check("%d %.2f", a, 5, "7 3.", 6);           // trunca, devuelve el largo total
    uint32_t b[] = {pi.w[0], pi.w[1], 9};               // double justo al principio (sin relleno)
    bad += check("%g/%u", b, 128, "3.25/9", 6);
    bad += check("%05.1f|%-3d|", (uint32_t[]){pi.w[0], pi.w[1], 1}, 128, "003.2|1  |", 10);
    if (!bad) printf("darwin_printf.c: ok\n");
    return bad;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src, exe = os.path.join(tmp, 'main.c'), os.path.join(tmp, 't')
    open(src, 'w').write(MAIN)
    subprocess.check_call(['cc', '-O2', '-Wall', '-I', os.path.join(ROOT, 'source'), src,
                           os.path.join(ROOT, 'source', 'darwin_printf.c'), '-o', exe])
    raise SystemExit(subprocess.call([exe]))
