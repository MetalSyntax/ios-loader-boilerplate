#!/usr/bin/env python3
"""Verifica source/cgbi.c en el host: lo compila con cc + zlib y compara, píxel a píxel, contra un
decodificador Python independiente sobre todos los PNG CgBI del bundle (game_app). No modifica
ningún archivo del juego.

Uso: python3 -I tools/cgbi_check.py [carpeta .app]
"""
import glob, os, struct, subprocess, sys, tempfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = r'''
#include <stdio.h>
#include <stdlib.h>
#include "cgbi.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    unsigned char *d = malloc(n); fread(d, 1, n, f); fclose(f);
    if (!cgbi_is(d, n)) return 2;
    int w, h, bpp; unsigned char *px = cgbi_decode(d, n, &w, &h, &bpp, malloc);
    if (!px) return 3;
    printf("%d %d %d\n", w, h, bpp); fwrite(px, 1, (size_t)w * h * 4, stdout);
    return 0;
}
'''


def reference(data):
    """RGBA premultiplicado de un CgBI (independiente del C)."""
    off, idat = 8, b''
    while off < len(data):
        n, typ = struct.unpack_from('>I4s', data, off)
        if typ == b'IHDR':
            w, h, _, ctype = struct.unpack_from('>IIBB', data, off + 8)
        elif typ == b'IDAT':
            idat += data[off + 8:off + 8 + n]
        off += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, stride = zlib.decompress(idat, -15), w * bpp
    out, prev = bytearray(), bytes(stride)
    for y in range(h):
        ft, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a, b = line[i - bpp] if i >= bpp else 0, prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            p = [0, a, b, (a + b) >> 1, 0][ft]
            if ft == 4:
                q = a + b - c
                p = a if abs(q - a) <= abs(q - b) and abs(q - a) <= abs(q - c) else b if abs(q - b) <= abs(q - c) else c
            line[i] = (line[i] + p) & 0xFF
        for x in range(w):
            s = line[x * bpp:x * bpp + bpp]
            out += bytes((s[2], s[1], s[0], s[3] if bpp == 4 else 255))
        prev = line
    return w, h, bytes(out)


def main():
    app = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'game_app')
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = os.path.join(tmp, 'main.c'), os.path.join(tmp, 'cgbi_test')
        open(src, 'w').write(MAIN)
        subprocess.check_call(['cc', '-O2', '-I', os.path.join(ROOT, 'source'), src,
                               os.path.join(ROOT, 'source', 'cgbi.c'), '-lz', '-o', exe])
        pngs = [p for p in glob.glob(os.path.join(app, '**', '*.png'), recursive=True)
                if not os.path.basename(p).startswith('._')]
        n = 0
        for p in sorted(pngs):
            data = open(p, 'rb').read()
            if data[12:16] != b'CgBI':
                continue
            r = subprocess.run([exe, p], capture_output=True)
            assert r.returncode == 0, f'{p}: cgbi.c devolvió {r.returncode}'
            head, px = r.stdout.split(b'\n', 1)
            w, h, _ = map(int, head.split())
            assert (w, h, px) == reference(data), f'{p}: difiere de la referencia'
            n += 1
        assert n, 'no hay PNG CgBI para probar'
        print(f'cgbi.c: {n} PNG CgBI idénticos a la referencia')


if __name__ == '__main__':
    main()
