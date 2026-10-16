// Loader de Mach-O 32 bits (MH_EXECUTE, ARMv6 / ARMv7) para PS Vita.
//
// 1. __TEXT se arma en memoria temporal y se copia a un bloque RX mediante kubridge;
//    __DATA se mapea a un bloque RW contiguo preservando la distancia de segmentos (PIC).
// 2. rebase.bin (generado por tools/macho_prep.py) lista los punteros absolutos en __DATA
//    a los que se suma MACHO_SLIDE.
// 3. Imports: resuelve imports dinámicos interceptando llamadas a bibliotecas de iOS / Darwin / ObjC.
//    Lo que no se encuentre va a un thunk automático que registra el nombre en log y retorna 0.
#include <kubridge.h>
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imports.h"
#include "log.h"
#include "macho.h"

#define LC_SEGMENT 0x1
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xB
#define LC_ENCRYPTION_INFO 0x21

#define S_ZEROFILL 0x1
#define S_NON_LAZY_SYMBOL_POINTERS 0x6
#define S_LAZY_SYMBOL_POINTERS 0x7

#define INDIRECT_SYMBOL_LOCAL 0x80000000u
#define INDIRECT_SYMBOL_ABS 0x40000000u

#define N_STAB 0xE0
#define N_TYPE 0x0E
#define N_SECT 0x0E
#define N_ARM_THUMB_DEF 0x0008

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050
#endif

#define ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))
#define THUNK_ARENA_SIZE 0x20000

typedef struct {
    char segname[16];
    uint32_t vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, flags;
} segment_command;

typedef struct {
    char sectname[16], segname[16];
    uint32_t addr, size, offset, align, reloff, nreloc, flags, reserved1, reserved2;
} section;

typedef struct {
    uint32_t n_strx;
    uint8_t n_type, n_sect;
    uint16_t n_desc;
    uint32_t n_value;
} nlist;

typedef struct {
    uint32_t ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym, tocoff, ntoc,
        modtaboff, nmodtab, extrefsymoff, nextrefsyms, indirectsymoff, nindirectsyms, extreloff,
        nextrel, locreloff, nlocrel;
} dysymtab_command;

static uint8_t *file_data;
static uint32_t file_slice_offset = 0;
static nlist *symtab;
static uint32_t nsyms;
static const char *strtab;
static uint32_t text_vm, text_size, data_vm, data_size;
static uint8_t *text_buf;
static uintptr_t text_real;
static uint32_t mod_init_addr, mod_init_count;

static uint32_t *thunk_head, *thunk_end;
static const char **unimpl_names;
static int unimpl_count;

extern void ml_unimpl_entry(void);
extern void ml_align_call(void);

void ml_unimpl_log(int idx) {
    static uint8_t seen[4096];
    if (idx < (int)sizeof(seen) && seen[idx] < 3) {
        seen[idx]++;
        l_warn("unimpl import: %s%s", unimpl_names[idx], seen[idx] == 3 ? " (silenciado)" : "");
    }
}

static uintptr_t make_thunk(uint32_t ip_value, void *jump) {
    if (thunk_head + 4 > thunk_end)
        fatal_error("Arena de thunks llena");
    uint32_t code[4] = {
        0xE59FC000, // ldr ip, [pc, #0]
        0xE59FF000, // ldr pc, [pc, #0]
        ip_value,
        (uint32_t)jump,
    };
    uintptr_t addr = (uintptr_t)thunk_head;
    kuKernelCpuUnrestrictedMemcpy(thunk_head, code, sizeof(code));
    kuKernelFlushCaches(thunk_head, sizeof(code));
    thunk_head += 4;
    return addr;
}

static uintptr_t make_unimpl_thunk(const char *name) {
    unimpl_names = realloc(unimpl_names, (unimpl_count + 1) * sizeof(char *));
    unimpl_names[unimpl_count] = name;
    return make_thunk(unimpl_count++, ml_unimpl_entry);
}

static uintptr_t make_unimpl_data(const char *name) {
    static struct {
        const char *name;
        uintptr_t addr;
    } blocks[512];
    static int nblocks;
    for (int i = 0; i < nblocks; i++)
        if (!strcmp(blocks[i].name, name))
            return blocks[i].addr;
    void *b = calloc(1, 256);
    if (nblocks < 512)
        blocks[nblocks++] = (typeof(blocks[0])){name, (uintptr_t)b};
    return (uintptr_t)b;
}

uintptr_t macho_sym(const char *name) {
    if (!symtab || !strtab)
        return 0;
    for (uint32_t i = 0; i < nsyms; i++) {
        nlist *s = &symtab[i];
        if (s->n_type & N_STAB)
            continue;
        if ((s->n_type & N_TYPE) != N_SECT)
            continue;
        if (!strcmp(strtab + s->n_strx, name)) {
            uintptr_t a = MACHO_SLIDE + s->n_value;
            if (s->n_desc & N_ARM_THUMB_DEF)
                a |= 1;
            return a;
        }
    }
    return 0;
}

void macho_hook(const char *name, void *dst) {
    uintptr_t a = macho_sym(name);
    if (!a) {
        l_warn("macho_hook: simbolo '%s' no encontrado", name);
        return;
    }
    uint32_t code[4] = {
        0xE59FC000, // ldr ip, [pc, #0]
        0xE59FF000, // ldr pc, [pc, #0]
        (uint32_t)dst,
        (uint32_t)dst,
    };
    kuKernelCpuUnrestrictedMemcpy((void *)(a & ~1), code, sizeof(code));
    kuKernelFlushCaches((void *)(a & ~1), sizeof(code));
}

static void apply_rebase(const char *rebase_path) {
    FILE *f = fopen(rebase_path, "rb");
    if (!f) {
        l_warn("No se encontro %s: omitiendo rebase offline", rebase_path);
        return;
    }
    uint32_t magic = 0, count = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x31534252 /* RBS1 */) {
        fclose(f);
        l_error("Formato de rebase.bin invalido (esperado RBS1)");
        return;
    }
    if (fread(&count, 4, 1, f) != 1) {
        fclose(f);
        return;
    }

    uint32_t *entries = malloc(count * sizeof(uint32_t));
    if (!entries) {
        fclose(f);
        return;
    }
    fread(entries, sizeof(uint32_t), count, f);
    fclose(f);

    l_info("Aplicando %u palabras de rebase desde %s...", (unsigned)count, rebase_path);
    int applied = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = entries[i];
        if (va >= data_vm && va < data_vm + data_size) {
            uint32_t *ptr = (uint32_t *)(MACHO_SLIDE + va);
            *ptr += MACHO_SLIDE;
            applied++;
        } else if (va >= text_vm && va < text_vm + text_size) {
            // Rebase en stubs de texto
            uint32_t *ptr = (uint32_t *)(text_buf + (va - text_vm));
            *ptr += MACHO_SLIDE;
            applied++;
        }
    }
    free(entries);
    l_info("Rebase completado: %d palabras reubicadas.", applied);
}

int macho_load(const char *bin_path, const char *rebase_path) {
    l_info("Cargando ejecutable Mach-O: %s", bin_path);
    FILE *f = fopen(bin_path, "rb");
    if (!f)
        fatal_error("No se pudo abrir %s", bin_path);
    fseek(f, 0, SEEK_END);
    size_t file_len = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *raw = malloc(file_len);
    if (!raw)
        fatal_error("No hay memoria suficiente para cargar %s", bin_path);
    fread(raw, 1, file_len, f);
    fclose(f);

    uint8_t *d = raw;
    file_slice_offset = 0;

    // Detectar FAT Mach-O (big-endian 0xCAFEBABE)
    if (file_len >= 8 && (memcmp(raw, "\xca\xfe\xba\xbe", 4) == 0 || memcmp(raw, "\xbe\xba\xfe\xca", 4) == 0)) {
        uint32_t nfat = (raw[4] << 24) | (raw[5] << 16) | (raw[6] << 8) | raw[7];
        uint32_t chosen_offset = 0, chosen_size = 0;
        const char *arch_name = "arm";
        for (uint32_t i = 0; i < nfat; i++) {
            const uint8_t *arch_ptr = raw + 8 + i * 20;
            uint32_t cputype = (arch_ptr[0] << 24) | (arch_ptr[1] << 16) | (arch_ptr[2] << 8) | arch_ptr[3];
            uint32_t cpusub = (arch_ptr[4] << 24) | (arch_ptr[5] << 16) | (arch_ptr[6] << 8) | arch_ptr[7];
            uint32_t off = (arch_ptr[8] << 24) | (arch_ptr[9] << 16) | (arch_ptr[10] << 8) | arch_ptr[11];
            uint32_t sz = (arch_ptr[12] << 24) | (arch_ptr[13] << 16) | (arch_ptr[14] << 8) | arch_ptr[15];
            if (cputype == 12 /* CPU_TYPE_ARM */) {
                if (cpusub == 9 /* v7 */ || cpusub == 11 /* v7s */) {
                    chosen_offset = off;
                    chosen_size = sz;
                    arch_name = "armv7";
                    break;
                } else if (cpusub == 6 /* v6 */ && !chosen_offset) {
                    chosen_offset = off;
                    chosen_size = sz;
                    arch_name = "armv6";
                }
            }
        }
        if (chosen_offset && chosen_offset + chosen_size <= file_len) {
            l_info("Binario FAT detectado: seleccionando slice %s (offset 0x%X, tamaño %u KB)",
                   arch_name, (unsigned)chosen_offset, (unsigned)(chosen_size / 1024));
            d = raw + chosen_offset;
            file_slice_offset = chosen_offset;
        }
    }

    file_data = d;
    uint32_t magic = *(uint32_t *)d;
    if (magic != 0xFEEDFACE)
        fatal_error("Encabezado Mach-O invalido (0x%08X != 0xFEEDFACE)", (unsigned)magic);

    uint32_t ncmds = *(uint32_t *)(d + 16);
    uint32_t off = 28;

    dysymtab_command *dysym = NULL;
    segment_command *seg_text = NULL, *seg_data = NULL;
    section *sects[64];
    int nsects_total = 0;

    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd = *(uint32_t *)(d + off);
        uint32_t size = *(uint32_t *)(d + off + 4);
        if (cmd == LC_SEGMENT) {
            segment_command *s = (segment_command *)(d + off);
            if (!strcmp(s->segname, "__TEXT"))
                seg_text = s;
            else if (!strcmp(s->segname, "__DATA"))
                seg_data = s;
            section *sec = (section *)(d + off + 56);
            for (uint32_t i = 0; i < s->nsects && nsects_total < 64; i++)
                sects[nsects_total++] = &sec[i];
        } else if (cmd == LC_SYMTAB) {
            uint32_t *p = (uint32_t *)(d + off + 8);
            symtab = (nlist *)(d + p[0]);
            nsyms = p[1];
            strtab = (const char *)(d + p[2]);
        } else if (cmd == LC_DYSYMTAB) {
            dysym = (dysymtab_command *)(d + off + 8);
        } else if (cmd == LC_ENCRYPTION_INFO) {
            uint32_t cryptid = *(uint32_t *)(d + off + 16);
            if (cryptid != 0) {
                l_error("================================================================");
                l_error("ALERTA CRITICA: El binario tiene cifrado FairPlay (cryptid %u)!", (unsigned)cryptid);
                l_error("No podra ejecutarse en PS Vita. Debe descifrarse antes.");
                l_error("================================================================");
            }
        }
        off += size;
    }

    if (!seg_text || !seg_data)
        fatal_error("Mach-O no contiene __TEXT o __DATA");

    text_vm = seg_text->vmaddr;
    text_size = ALIGN(seg_text->vmsize, 0x1000);
    data_vm = seg_data->vmaddr;
    data_size = ALIGN(seg_data->vmsize, 0x1000);

    l_info("__TEXT vm: 0x%08X (size 0x%08X) -> real 0x%08X",
           (unsigned)text_vm, (unsigned)text_size, (unsigned)(MACHO_SLIDE + text_vm));
    l_info("__DATA vm: 0x%08X (size 0x%08X) -> real 0x%08X",
           (unsigned)data_vm, (unsigned)data_size, (unsigned)(MACHO_SLIDE + data_vm));

    // Mapeo de bloques de memoria
    SceUID text_blk = sceKernelAllocMemBlockForVM("macho_text", text_size);
    if (text_blk < 0)
        fatal_error("sceKernelAllocMemBlockForVM(text) = 0x%08X", (unsigned)text_blk);
    void *text_ptr = NULL;
    sceKernelGetMemBlockBase(text_blk, &text_ptr);
    text_real = (uintptr_t)text_ptr;

    SceUID data_blk = sceKernelAllocMemBlockForVM("macho_data", data_size);
    if (data_blk < 0)
        fatal_error("sceKernelAllocMemBlockForVM(data) = 0x%08X", (unsigned)data_blk);
    void *data_ptr = NULL;
    sceKernelGetMemBlockBase(data_blk, &data_ptr);

    // Arena para thunks ejecutables
    SceUID thunk_blk = sceKernelAllocMemBlockForVM("macho_thunks", THUNK_ARENA_SIZE);
    void *thunk_ptr = NULL;
    if (thunk_blk >= 0) {
        sceKernelGetMemBlockBase(thunk_blk, &thunk_ptr);
        thunk_head = (uint32_t *)thunk_ptr;
        thunk_end = thunk_head + (THUNK_ARENA_SIZE / 4);
    }

    // Copiar datos de segmentos
    text_buf = malloc(text_size);
    memset(text_buf, 0, text_size);
    memcpy(text_buf, d + seg_text->fileoff, seg_text->filesize);

    memset(data_ptr, 0, data_size);
    memcpy(data_ptr, d + seg_data->fileoff, seg_data->filesize);

    // Inicializar secciones especiales
    for (int i = 0; i < nsects_total; i++) {
        section *s = sects[i];
        if (!strcmp(s->sectname, "__mod_init_func")) {
            mod_init_addr = MACHO_SLIDE + s->addr;
            mod_init_count = s->size / 4;
        }
    }

    // Resolver imports (indirect symbols y symbol pointers)
    if (dysym) {
        uint32_t *indirect = (uint32_t *)(d + dysym->indirectsymoff);
        for (int s_idx = 0; s_idx < nsects_total; s_idx++) {
            section *s = sects[s_idx];
            if (s->type == S_NON_LAZY_SYMBOL_POINTERS || s->type == S_LAZY_SYMBOL_POINTERS) {
                uint32_t nslots = s->size / 4;
                uint32_t *slots = (uint32_t *)(MACHO_SLIDE + s->addr);
                for (uint32_t i = 0; i < nslots; i++) {
                    uint32_t sym_idx = indirect[s->r1 + i];
                    if (sym_idx & INDIRECT_SYMBOL_LOCAL) {
                        slots[i] += MACHO_SLIDE;
                    } else if (!(sym_idx & INDIRECT_SYMBOL_ABS)) {
                        const char *name = strtab + symtab[sym_idx].n_strx;
                        const import_entry *imp = imports_lookup(name);
                        if (imp) {
                            if (imp->data) {
                                slots[i] = imp->addr;
                            } else {
                                slots[i] = make_thunk(imp->addr, ml_align_call);
                            }
                        } else {
                            slots[i] = make_unimpl_thunk(name);
                        }
                    }
                }
            }
        }
    }

    // Aplicar rebase offline
    if (rebase_path)
        apply_rebase(rebase_path);

    // Copiar __TEXT final al bloque ejecutable
    kuKernelCpuUnrestrictedMemcpy((void *)(MACHO_SLIDE + text_vm), text_buf, text_size);
    kuKernelFlushCaches((void *)(MACHO_SLIDE + text_vm), text_size);
    free(text_buf);
    text_buf = NULL;

    l_info("Mach-O cargado exitosamente.");
    return 0;
}

void macho_run_inits(void) {
    if (!mod_init_addr || !mod_init_count)
        return;
    l_info("Ejecutando %u constructores globales (__mod_init_func)...", (unsigned)mod_init_count);
    uint32_t *funcs = (uint32_t *)mod_init_addr;
    for (uint32_t i = 0; i < mod_init_count; i++) {
        if (funcs[i]) {
            uintptr_t fn = funcs[i] + MACHO_SLIDE;
            call_guest((void *)fn, 0, 0, 0, 0);
        }
    }
}
