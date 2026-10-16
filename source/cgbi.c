/**
 * @file cgbi.c
 * @brief Apple CgBI PNG decoder.
 * @details Decodes iOS bundle PNGs processed by Apple's proprietary pngcrush:
 *          has a 'CgBI' chunk before IHDR, raw deflate IDAT streams (without zlib header),
 *          and BGR(A) pixel order with premultiplied alpha. Standard libpng rejects them;
 *          they are decoded directly here as shipped inside the IPA package.
 *          Self-contained with zero Vita dependencies (only zlib) for host testing.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "cgbi.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int cgbi_is(const void *data, int len) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return len > 16 && !memcmp(data, sig, 8) && !memcmp((const uint8_t *)data + 12, "CgBI", 4);
}

static void unfilter(uint8_t *raw, int w, int h, int bpp, uint8_t *out) {
    int stride = w * bpp;
    const uint8_t *prev = NULL;
    for (int y = 0; y < h; y++) {
        int ft = raw[y * (stride + 1)];
        uint8_t *line = raw + y * (stride + 1) + 1;
        for (int i = 0; i < stride; i++) {
            int a = i >= bpp ? line[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int p = 0;
            if (ft == 1) {
                p = a;
            } else if (ft == 2) {
                p = b;
            } else if (ft == 3) {
                p = (a + b) >> 1;
            } else if (ft == 4) {
                int q = a + b - c, pa = abs(q - a), pb = abs(q - b), pc = abs(q - c);
                p = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
            }
            line[i] = (uint8_t)(line[i] + p);
        }
        memcpy(out + y * stride, line, stride);
        prev = line;
    }
}

uint8_t *cgbi_decode(const void *data, int len, int *w, int *h, int *bpp, void *(*alloc)(size_t)) {
    const uint8_t *d = data, *end = d + len, *ihdr = NULL;
    size_t idat_len = 0;
    for (const uint8_t *p = d + 8; p + 12 <= end; p += 12 + be32(p)) {
        if (!memcmp(p + 4, "IHDR", 4))
            ihdr = p + 8;
        else if (!memcmp(p + 4, "IDAT", 4))
            idat_len += be32(p);
    }
    // 8-bit, RGB (2) or RGBA (6), non-interlaced: standard Apple pngcrush output.
    if (!ihdr || ihdr[8] != 8 || (ihdr[9] != 2 && ihdr[9] != 6) || ihdr[12])
        return NULL;
    *w = be32(ihdr);
    *h = be32(ihdr + 4);
    int src_bpp = ihdr[9] == 6 ? 4 : 3, stride = *w * src_bpp;
    *bpp = src_bpp * 8;

    uint8_t *idat = malloc(idat_len), *q = idat;
    for (const uint8_t *p = d + 8; p + 12 <= end; p += 12 + be32(p))
        if (!memcmp(p + 4, "IDAT", 4)) {
            memcpy(q, p + 8, be32(p));
            q += be32(p);
        }
    uLongf raw_len = (uLongf)(stride + 1) * *h;
    uint8_t *raw = malloc(raw_len), *px = malloc((size_t)stride * *h), *out = NULL;
    z_stream z = {0};
    z.next_in = idat;
    z.avail_in = idat_len;
    z.next_out = raw;
    z.avail_out = raw_len;
    int ok = inflateInit2(&z, -15) == Z_OK && inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == raw_len;
    inflateEnd(&z);
    if (ok) {
        unfilter(raw, *w, *h, src_bpp, px);
        out = alloc((size_t)*w * *h * 4);
        for (int i = 0; out && i < *w * *h; i++) {
            const uint8_t *s = px + i * src_bpp;
            out[i * 4 + 0] = s[2];
            out[i * 4 + 1] = s[1];
            out[i * 4 + 2] = s[0];
            out[i * 4 + 3] = src_bpp == 4 ? s[3] : 255;
        }
    }
    free(idat);
    free(raw);
    free(px);
    return out;
}
