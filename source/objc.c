// Mini runtime ObjC: no hay clases ni métodos, solo un despacho por nombre de selector (los
// selrefs apuntan al nombre en __objc_methname) para lo poco de Foundation/UIKit que el motor
// necesita de verdad:
//   - texto: ___utilGetStringWidth/___utilMakeString usan NSString + UIFont (sizeWithFont:,
//     drawAtPoint:withFont:) sobre un CGBitmapContext gris de 512x32 (atlas de glifos). Se
//     dibuja con FreeType sobre ese mismo buffer: el resto del motor (quads, ___utilFontOut)
//     queda intacto.
//   - rutas/versión/fecha: resourcePath (FMOD), objectForInfoDictionaryKey:, NSCalendar.
//   - entrada de texto (nombre del personaje): textInput* de EAGLView -> sceImeDialog.
// Todo lo demás devuelve nil/0 como antes, registrando el selector la primera vez.
//
// Los NSString propios tienen el layout de un __cfstring {isa, flags, cstr, len}: así las
// constantes @"..." del binario y los creados acá se leen igual (cstr en +8).
#include <ft2build.h>
#include FT_FREETYPE_H
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "darwin_printf.h"
#include "hle.h"
#include "imports.h"
#include "log.h"
#include "macho.h"

// ---------------------------------------------------------------- NSString

typedef struct {
    void *isa;
    uint32_t flags;
    char *str;
    uint32_t len;
} ns_string;

// ponytail: los NSString son autorelease en iOS; acá viven en un anillo de 256 (el motor solo
// los usa dentro de la misma función). Si algún día uno se guarda, pasarlo a malloc propio.
static ns_string *ns_new(const char *s) {
    static ns_string ring[256];
    static int next;
    ns_string *o = &ring[next++ & 255];
    free(o->str);
    o->str = strdup(s ? s : "");
    o->len = strlen(o->str);
    o->flags = 0x7C8;
    return o;
}

static const char *ns_cstr(const void *o) { return o ? ((const ns_string *)o)->str : ""; }

static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= extra ? 0x3F >> extra : 0xFF;
    while (extra-- && (*s & 0xC0) == 0x80)
        c = (c << 6) | (*s++ & 0x3F);
    *p = s;
    return c;
}

static int utf16_len(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
        n += utf8_next(&p) > 0xFFFF ? 2 : 1;
    return n;
}

// stringWithFormat: con el printf de Darwin (%@, doubles en 2 palabras).
static ns_string *ns_format(const char *fmt, va_list ap) {
    char out[1024];
    va_list cp;
    va_copy(cp, ap);
    words w = {&cp, NULL};
    darwin_vformat(out, sizeof(out), fmt, &w);
    va_end(cp);
    return ns_new(out);
}

// ---------------------------------------------------------------- fuentes (FreeType)

typedef struct {
    uint32_t key; // tamaño << 24 | código
    int16_t left, top, adv;
    uint16_t w, h;
    uint8_t *bmp;
} glyph;

#define GLYPH_CACHE 4096
static glyph cache[GLYPH_CACHE];
static FT_Library ft;
static FT_Face faces[4];
static int nfaces = -1;

static void fonts_init(void) {
    // font.ttf propio primero (por si el sistema no tiene algún alfabeto), después los del sistema.
    static const char *paths[] = {DATA_PATH "font.ttf", "sa0:data/font/pvf/ltn0.pvf",
                                  "sa0:data/font/pvf/kr0.pvf", "sa0:data/font/pvf/jpn0.pvf"};
    nfaces = 0;
    if (FT_Init_FreeType(&ft))
        return l_error("FreeType no inicializa: sin texto");
    for (int i = 0; i < 4; i++)
        if (!FT_New_Face(ft, paths[i], 0, &faces[nfaces])) {
            l_info("fuente: %s", paths[i]);
            nfaces++;
        }
    if (!nfaces)
        l_error("ninguna fuente cargó: sin texto");
}

static const glyph *get_glyph(int size, uint32_t cp) {
    static glyph tmp;
    uint32_t key = (uint32_t)size << 24 | (cp & 0xFFFFFF);
    uint32_t h = (key * 2654435761u) % GLYPH_CACHE;
    glyph *g = NULL;
    for (int i = 0; i < 16; i++) {
        glyph *c = &cache[(h + i) % GLYPH_CACHE];
        if (c->key == key)
            return c;
        if (!c->key) {
            g = c;
            break;
        }
    }
    if (!g) { // caché llena en esa zona: se rasteriza igual, sin guardar
        free(tmp.bmp);
        g = &tmp;
    }
    memset(g, 0, sizeof(*g));
    g->key = key;
    if (nfaces < 0)
        fonts_init();
    FT_Face f = NULL;
    for (int i = 0; i < nfaces && !f; i++)
        if (FT_Get_Char_Index(faces[i], cp))
            f = faces[i];
    if (!f) {
        g->adv = cp == ' ' ? size / 3 : size / 2;
        return g;
    }
    FT_Set_Pixel_Sizes(f, 0, size);
    if (FT_Load_Char(f, cp, FT_LOAD_RENDER))
        return g;
    FT_GlyphSlot s = f->glyph;
    g->left = s->bitmap_left;
    g->top = s->bitmap_top;
    g->adv = s->advance.x >> 6;
    g->w = s->bitmap.width;
    g->h = s->bitmap.rows;
    g->bmp = malloc(g->w * g->h + 1);
    for (int y = 0; y < g->h; y++)
        memcpy(g->bmp + y * g->w, s->bitmap.buffer + y * s->bitmap.pitch, g->w);
    return g;
}

// ponytail: alto de línea y línea base aproximan la Helvetica de iOS 3 (14pt->18, 13->16,
// 11->14). Si el texto queda corrido verticalmente en consola, se ajusta solo esto.
static int line_height(int size) { return size == 14 ? 18 : size == 13 ? 16 : size == 11 ? 14 : (size * 5 + 2) / 4; }
static int baseline(int size) { return (line_height(size) - size) / 2 + (size * 4 + 2) / 5; }

static int text_width(int size, const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;)
        w += get_glyph(size, utf8_next(&p))->adv;
    return w;
}

// ---------------------------------------------------------------- CoreGraphics / UIKit

typedef struct {
    uint8_t *data;
    int w, h, bpr;
    float gray;
} cg_ctx;

static cg_ctx *cur_ctx;

static cg_ctx *cg_BitmapContextCreate(void *data, int w, int h, int bpc, int bpr, void *cs, int info) {
    cg_ctx *c = calloc(1, sizeof(*c));
    *c = (cg_ctx){data, w, h, bpr, 1.0f};
    return c;
}
static void cg_ContextRelease(cg_ctx *c) {
    if (cur_ctx == c)
        cur_ctx = NULL;
    free(c);
}
SOFTFP static void cg_SetGrayFillColor(cg_ctx *c, float gray, float alpha) { c->gray = gray * alpha; }
static void *cg_ColorSpaceCreate(void) { return (void *)1; }
static void cg_noop(void) {}
static void ui_PushContext(cg_ctx *c) { cur_ctx = c; }
static void ui_PopContext(void) { cur_ctx = NULL; }

typedef struct {
    uint32_t tag;
    int size;
} ui_font;

static ui_font *font_of_size(float pt) {
    static ui_font fonts[32];
    int size = (int)pt;
    for (int i = 0; i < 32; i++)
        if (!fonts[i].tag || fonts[i].size == size) {
            fonts[i] = (ui_font){0x464F4E54, size};
            return &fonts[i];
        }
    return &fonts[31];
}

// UIKit dibuja con el eje y hacia abajo; el contexto del atlas no está invertido, así que el texto
// queda cabeza abajo en memoria (fila j del glifo -> fila h-1-j), que es lo que espera el motor.
static void draw_text(cg_ctx *c, const char *s, int x, int y, int size) {
    int pen = x, base = baseline(size);
    uint8_t ink = (uint8_t)(c->gray * 255.0f);
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        const glyph *g = get_glyph(size, utf8_next(&p));
        for (int gy = 0; gy < g->h; gy++) {
            int row = c->h - 1 - (y + base - g->top + gy);
            if (row < 0 || row >= c->h)
                continue;
            for (int gx = 0; gx < g->w; gx++) {
                int col = pen + g->left + gx;
                if (col < 0 || col >= c->w)
                    continue;
                uint8_t v = g->bmp[gy * g->w + gx] * ink / 255, *d = &c->data[row * c->bpr + col];
                if (v > *d)
                    *d = v;
            }
        }
        pen += g->adv;
    }
}

// ---------------------------------------------------------------- entrada de texto (IME)

static int ime_type = -1;
static char ime_utf8[128];
static uint16_t ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], ime_title[16], ime_initial[1];

static void ime_open(int type) {
    static const char *titles[] = {"Name", "Number"};
    const char *t = titles[type == 1];
    for (int i = 0; (ime_title[i] = t[i]); i++) {}
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.type = type == 1 ? SCE_IME_TYPE_NUMBER : SCE_IME_TYPE_DEFAULT;
    p.title = ime_title;
    p.maxTextLength = type == 1 ? 4 : 12;
    p.initialText = ime_initial;
    p.inputTextBuffer = ime_buf;
    ime_utf8[0] = 0;
    int r = sceImeDialogInit(&p);
    if (r < 0)
        l_error("sceImeDialogInit = 0x%08X", r);
    ime_type = type; // aunque falle: ime_poll cierra el evento y el juego no queda esperando
}

int ime_active(void) { return ime_type >= 0; }

void ime_poll(void) {
    if (ime_type < 0)
        return;
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_RUNNING)
        return;
    if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult res;
        memset(&res, 0, sizeof(res));
        sceImeDialogGetResult(&res);
        if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
            size_t n = 0;
            for (int i = 0; ime_buf[i] && n < sizeof(ime_utf8) - 4; i++) {
                uint16_t c = ime_buf[i];
                if (c < 0x80)
                    ime_utf8[n++] = c;
                else if (c < 0x800)
                    n += sprintf(ime_utf8 + n, "%c%c", 0xC0 | c >> 6, 0x80 | (c & 0x3F));
                else
                    n += sprintf(ime_utf8 + n, "%c%c%c", 0xE0 | c >> 12, 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
            }
            ime_utf8[n] = 0;
        }
        sceImeDialogTerm();
    }
    int type = ime_type;
    ime_type = -1;
    l_info("texto ingresado (%d): \"%s\"", type, ime_utf8);
    uintptr_t text_close_fn = macho_sym("_Ex_HandleEventTextClose");
    if (text_close_fn)
        call_guest((void *)text_close_fn, type, 0, 0, 0);
}

// ---------------------------------------------------------------- objc_msgSend

static uint32_t dummy_obj[4];
static uint32_t ret_obj(void) { return (uint32_t)dummy_obj; }

static void log_unknown(void *self, const char *sel) {
    static const char *seen[512];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == sel)
            return;
    if (nseen < 512)
        seen[nseen++] = sel;
    l_error("objc_msgSend(%p, %s) -> nil", self, sel);
}

static int sel_is(const char *sel, const char *name) { return !strcmp(sel, name); }

static uint32_t objc_msgSend(void *self, const char *sel, ...) {
    va_list ap;
    va_start(ap, sel);
    uint32_t a = va_arg(ap, uint32_t); // primer argumento (si lo hay)
    uint32_t r = 0;
    const struct tm *now;
    time_t t;
    if (sel_is(sel, "stringWithUTF8String:") || sel_is(sel, "stringWithCString:encoding:")) {
        r = (uint32_t)ns_new((const char *)a);
    } else if (sel_is(sel, "stringWithFormat:")) {
        r = (uint32_t)ns_format(ns_cstr((void *)a), ap);
    } else if (sel_is(sel, "length")) {
        r = utf16_len(ns_cstr(self));
    } else if (sel_is(sel, "UTF8String") || sel_is(sel, "cStringUsingEncoding:")) {
        r = (uint32_t)ns_cstr(self);
    } else if (sel_is(sel, "getCString:maxLength:encoding:")) {
        uint32_t max = va_arg(ap, uint32_t);
        snprintf((char *)a, max, "%s", ns_cstr(self));
        r = 1;
    } else if (sel_is(sel, "systemFontOfSize:")) {
        union { uint32_t u; float f; } v = {a};
        r = (uint32_t)font_of_size(v.f);
    } else if (sel_is(sel, "mainBundle") || sel_is(sel, "currentCalendar") || sel_is(sel, "date") ||
               sel_is(sel, "components:fromDate:")) {
        r = ret_obj();
    } else if (sel_is(sel, "resourcePath")) {
        char p[256];
        snprintf(p, sizeof(p), "%s", BUNDLE_PATH);
        p[strlen(p) - 1] = 0; // sin la barra final, como NSBundle
        r = (uint32_t)ns_new(p);
    } else if (sel_is(sel, "objectForInfoDictionaryKey:")) {
        const char *k = ns_cstr((void *)a);
        r = !strcmp(k, "CFBundleVersion") || !strcmp(k, "CFBundleShortVersionString") ? (uint32_t)ns_new("1.1.41") : 0;
    } else if (sel_is(sel, "year") || sel_is(sel, "month") || sel_is(sel, "day")) {
        t = time(NULL);
        now = localtime(&t);
        r = sel[0] == 'y' ? now->tm_year + 1900 : sel[0] == 'm' ? now->tm_mon + 1 : now->tm_mday;
    } else if (sel_is(sel, "textInputOpen:")) {
        ime_open(a);
    } else if (sel_is(sel, "textInputGetUTF8")) {
        r = (uint32_t)ime_utf8;
    } else if (sel_is(sel, "textInputGetLength")) {
        r = utf16_len(ime_utf8);
    } else {
        log_unknown(self, sel);
    }
    va_end(ap);
    return r;
}

// Métodos que devuelven structs (CGSize/CGPoint): el primer argumento es el buffer de retorno.
static void objc_msgSend_stret(float *ret, void *self, const char *sel, ...) {
    va_list ap;
    va_start(ap, sel);
    if (sel_is(sel, "sizeWithFont:")) {
        ui_font *f = va_arg(ap, ui_font *);
        int size = f ? f->size : 14;
        ret[0] = text_width(size, ns_cstr(self));
        ret[1] = line_height(size);
    } else if (sel_is(sel, "drawAtPoint:withFont:")) {
        union { uint32_t u; float f; } x = {va_arg(ap, uint32_t)}, y = {va_arg(ap, uint32_t)};
        ui_font *f = va_arg(ap, ui_font *);
        int size = f ? f->size : 14;
        if (cur_ctx)
            draw_text(cur_ctx, ns_cstr(self), (int)x.f, (int)y.f, size);
        ret[0] = text_width(size, ns_cstr(self));
        ret[1] = line_height(size);
    } else {
        log_unknown(self, sel);
        ret[0] = ret[1] = 0;
    }
    va_end(ap);
}

// ---------------------------------------------------------------- tabla

#define F(name, fn) {"_" #name, (uintptr_t)(fn)}

const import_entry objc_imports[] = {
    F(objc_msgSend, objc_msgSend), F(objc_msgSend_stret, objc_msgSend_stret),
    F(CGBitmapContextCreate, cg_BitmapContextCreate), F(CGContextRelease, cg_ContextRelease),
    F(CGContextSetGrayFillColor, cg_SetGrayFillColor), F(CGContextSetTextDrawingMode, cg_noop),
    F(CGColorSpaceCreateDeviceGray, cg_ColorSpaceCreate), F(CGColorSpaceCreateDeviceRGB, cg_ColorSpaceCreate),
    F(CGColorSpaceRelease, cg_noop), F(UIGraphicsPushContext, ui_PushContext),
    F(UIGraphicsPopContext, ui_PopContext),
};
const int objc_imports_count = sizeof(objc_imports) / sizeof(objc_imports[0]);
