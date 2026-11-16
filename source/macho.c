/**
 * @file macho.c
 * @brief 32-bit Mach-O loader (MH_EXECUTE, ARMv6 / ARMv7) for PlayStation Vita.
 * @details
 * 1. Segments are mapped at their exact fixed virtual addresses (vmaddr + MACHO_SLIDE)
 *    via kuKernelAllocMemBlock, preserving inter-segment distance (PIC/non-PIC).
 * 2. Thunk arena is allocated as RX immediately preceding the image.
 * 3. Dynamic imports from LC_DYSYMTAB are resolved against native/HLE stubs or logged.
 * 4. rebase.bin relocations are applied.
 * 5. __TEXT is safely flushed to the RX memory block via kuKernelCpuUnrestrictedMemcpy.
 */
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
#define LC_UNIXTHREAD 0x5
#define LC_MAIN 0x80000028
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

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RW
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0C20D060
#endif

#define ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))
#define THUNK_ARENA_SIZE 0x20000
#define MAX_SEGS 16
#define MAX_SECTS 96

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
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
    int16_t n_desc;
    uint32_t n_value;
} nlist;

typedef struct {
    uint32_t ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym;
    uint32_t tocoff, ntoc, modtaboff, nmodtab, extrefsymoff, nextrefsyms;
    uint32_t indirectsymoff, nindirectsyms, extreloff, nextrel, locreloff, nlocrel;
} dysymtab_command;

static const uint8_t *file_data;
static uint32_t file_slice_offset;
static uint32_t text_vm, text_size;
static uint8_t *text_buf;
static const nlist *symtab;
static uint32_t nsyms;
static const char *strtab;
static dysymtab_command *dysym;
static uint32_t mod_init_addr, mod_init_count;
static uintptr_t macho_entry_addr = 0;

static uint32_t *thunk_head;
static uint32_t *thunk_end;

static const char **unimpl_names;
static int unimpl_count;

// Alignment helper for iOS 4-byte stack calls entering Vita runtime
extern void ml_unimpl_entry(void);
extern void ml_align_call(void);

void ml_unimpl_log(int idx) {
    static uint8_t seen[4096];
    if (idx < (int)sizeof(seen) && seen[idx] < 3) {
        seen[idx]++;
        l_warn("unimpl import: %s%s", unimpl_names[idx], seen[idx] == 3 ? " (silenced)" : "");
    }
}

static int alloc_block(const char *name, int type, uintptr_t addr, uint32_t size) {
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = 0x1;
    opt.field_C = addr;
    SceUID id = kuKernelAllocMemBlock(name, type, size, &opt);
    if (id < 0) {
        l_error("kuKernelAllocMemBlock(\"%s\", type=0x%08X, size=0x%X, addr=0x%08X) failed: 0x%08X",
                name, (unsigned)type, (unsigned)size, (unsigned)addr, (unsigned)id);
        return id;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(id, &base);
    if ((uintptr_t)base != addr) {
        l_error("%s allocated at %p instead of 0x%08X", name, base, (unsigned)addr);
        return -1;
    }
    return 0;
}

static uintptr_t make_thunk(uint32_t ip_value, void *jump) {
    if (thunk_head + 4 > thunk_end)
        fatal_error("Thunk arena exhausted");
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

static int is_data_symbol(const char *name) {
    if (!name) return 0;
    if (strncmp(name, "_OBJC_CLASS_$_", 14) == 0) return 1;
    if (strncmp(name, "_OBJC_METACLASS_$_", 18) == 0) return 1;
    if (strncmp(name, "_OBJC_IVAR_$_", 13) == 0) return 1;
    if (strncmp(name, "__objc_", 7) == 0) return 1;
    if (strncmp(name, "___CF", 5) == 0) return 1;
    if (strncmp(name, "_kCF", 4) == 0) return 1;
    if (strncmp(name, "_kEAGL", 6) == 0) return 1;
    if (strncmp(name, "__ZTV", 5) == 0) return 1;
    return 0;
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
        const nlist *s = &symtab[i];
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

uintptr_t macho_entry(void) {
    return macho_entry_addr;
}

void macho_hook_addr(uint32_t vm, void *dst) {
    uint32_t patch[2];
    uint32_t addr = vm & ~1;
    if (vm & 1) {
        if (addr & 2) {
            uint16_t nop = 0xBF00;
            kuKernelCpuUnrestrictedMemcpy((void *)addr, &nop, 2);
            addr += 2;
        }
        patch[0] = 0xF000F8DF; // ldr.w pc, [pc] (Thumb-2)
    } else {
        patch[0] = 0xE51FF004; // ldr pc, [pc, #-4] (ARM)
    }
    patch[1] = make_thunk((uint32_t)dst, ml_align_call);
    kuKernelCpuUnrestrictedMemcpy((void *)addr, patch, sizeof(patch));
    kuKernelFlushCaches((void *)addr, sizeof(patch));
}

void macho_hook(const char *name, void *dst) {
    uintptr_t a = macho_sym(name);
    if (!a) {
        l_warn("macho_hook: symbol '%s' not found", name);
        return;
    }
    macho_hook_addr(a, dst);
}

static void apply_rebase(const char *rebase_path) {
    FILE *f = fopen(rebase_path, "rb");
    if (!f) {
        l_warn("Not found %s: skipping offline rebase", rebase_path);
        return;
    }
    uint32_t magic = 0, count = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x31534252 /* RBS1 */) {
        fclose(f);
        l_error("Invalid rebase.bin header format (expected RBS1)");
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

    l_info("Applying %u rebase words from %s...", (unsigned)count, rebase_path);
    int applied = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = entries[i];
        if (va >= text_vm && va < text_vm + text_size) {
            // Rebase on text section stubs before copying to RX block
            uint32_t *ptr = (uint32_t *)(text_buf + (va - text_vm));
            *ptr += MACHO_SLIDE;
            applied++;
        } else {
            // Rebase in data segments (already mapped at MACHO_SLIDE + va)
            uint32_t *ptr = (uint32_t *)(MACHO_SLIDE + va);
            *ptr += MACHO_SLIDE;
            applied++;
        }
    }
    free(entries);
    l_info("Rebase complete: %d words relocated.", applied);
}

int macho_load(const char *bin_path, const char *rebase_path) {
    l_info("Loading Mach-O executable: %s", bin_path);
    FILE *f = fopen(bin_path, "rb");
    if (!f)
        fatal_error("Failed to open %s", bin_path);
    fseek(f, 0, SEEK_END);
    size_t file_len = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *raw = malloc(file_len);
    if (!raw)
        fatal_error("Insufficient memory to load %s", bin_path);
    fread(raw, 1, file_len, f);
    fclose(f);

    uint8_t *d = raw;
    file_slice_offset = 0;
    macho_entry_addr = 0;

    // Detect FAT universal Mach-O (big-endian 0xCAFEBABE)
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
            l_info("FAT binary detected: selected slice %s (offset 0x%X, size %u KB)",
                   arch_name, (unsigned)chosen_offset, (unsigned)(chosen_size / 1024));
            d = raw + chosen_offset;
            file_slice_offset = chosen_offset;
        }
    }

    file_data = d;
    uint32_t magic = *(uint32_t *)d;
    if (magic != 0xFEEDFACE)
        fatal_error("Invalid Mach-O 32-bit magic: 0x%08X (expected 0xFEEDFACE)", (unsigned)magic);

    uint32_t ncmds = *(uint32_t *)(d + 16);
    uint32_t off = 28;

    segment_command *segs[MAX_SEGS];
    int nseg = 0;
    uint32_t img_lo = 0xFFFFFFFF;
    uint32_t img_hi = 0;
    segment_command *seg_text = NULL;

    section *sects[MAX_SECTS];
    int nsects_total = 0;

    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd = *(uint32_t *)(d + off);
        uint32_t size = *(uint32_t *)(d + off + 4);
        if (cmd == LC_SEGMENT) {
            segment_command *s = (segment_command *)(d + off);
            if (strncmp(s->segname, "__PAGEZERO", 16) != 0 &&
                strncmp(s->segname, "__LINKEDIT", 16) != 0 &&
                s->vmsize > 0 && nseg < MAX_SEGS) {
                segs[nseg++] = s;
                if (s->vmaddr < img_lo)
                    img_lo = s->vmaddr;
                if (s->vmaddr + s->vmsize > img_hi)
                    img_hi = s->vmaddr + s->vmsize;
            }
            if (strncmp(s->segname, "__TEXT", 16) == 0) {
                seg_text = s;
                text_vm = s->vmaddr;
                text_size = ALIGN(s->vmsize, 0x1000);
            }
            section *sec = (section *)(s + 1);
            for (uint32_t i = 0; i < s->nsects && nsects_total < MAX_SECTS; i++) {
                sects[nsects_total++] = &sec[i];
            }
        } else if (cmd == LC_SYMTAB) {
            uint32_t *p = (uint32_t *)(d + off + 8);
            symtab = (const nlist *)(d + p[0]);
            nsyms = p[1];
            strtab = (const char *)(d + p[2]);
        } else if (cmd == LC_DYSYMTAB) {
            dysym = (dysymtab_command *)(d + off + 8);
        } else if (cmd == LC_MAIN) {
            uint64_t entryoff = *(uint64_t *)(d + off + 8);
            macho_entry_addr = (uintptr_t)entryoff;
            l_info("LC_MAIN detected: entryoff = 0x%08X", (unsigned)entryoff);
        } else if (cmd == LC_UNIXTHREAD) {
            uint32_t *regs = (uint32_t *)(d + off + 16);
            macho_entry_addr = regs[15];
            l_info("LC_UNIXTHREAD detected: entry pc = 0x%08X", (unsigned)macho_entry_addr);
        } else if (cmd == LC_ENCRYPTION_INFO) {
            uint32_t cryptid = *(uint32_t *)(d + off + 16);
            if (cryptid != 0) {
                l_error("================================================================");
                l_error("CRITICAL ALERT: Binary contains FairPlay DRM encryption (cryptid %u)!", (unsigned)cryptid);
                l_error("Cannot execute encrypted binary on PS Vita. Must be decrypted first.");
                l_error("================================================================");
            }
        }
        off += size;
    }

    if (!seg_text)
        fatal_error("Mach-O does not contain __TEXT segment");

    if (macho_entry_addr) {
        if (macho_entry_addr < 0x1000000) {
            macho_entry_addr = MACHO_SLIDE + text_vm + macho_entry_addr;
        } else {
            macho_entry_addr = MACHO_SLIDE + macho_entry_addr;
        }
        l_info("Primary Mach-O entry point resolved at 0x%08X", (unsigned)macho_entry_addr);
    }

    // 1. Executable thunk arena (placed right before the image)
    uintptr_t thunk_addr = ((img_lo & ~0xFFF) + MACHO_SLIDE - THUNK_ARENA_SIZE) & ~0xFFF;
    if (alloc_block("ml_thunks", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, thunk_addr, THUNK_ARENA_SIZE) < 0) {
        fatal_error("kuKernelAllocMemBlock failed for thunk arena at 0x%08X (verify kubridge.skprx)", (unsigned)thunk_addr);
    }
    thunk_head = (uint32_t *)thunk_addr;
    thunk_end = thunk_head + (THUNK_ARENA_SIZE / 4);

    // 2. Map all segments at exact vmaddr + MACHO_SLIDE
    for (int i = 0; i < nseg; i++) {
        segment_command *s = segs[i];
        int is_text = (strncmp(s->segname, "__TEXT", 16) == 0);
        uint32_t start = s->vmaddr & ~0xFFF;
        uint32_t end = ALIGN(s->vmaddr + s->vmsize, 0x1000);
        uint32_t blk_size = end - start;
        uintptr_t blk_addr = start + MACHO_SLIDE;

        l_info("Mapping segment %.16s (0x%08X-0x%08X, size 0x%X) -> 0x%08X (%s)",
               s->segname, (unsigned)s->vmaddr, (unsigned)(s->vmaddr + s->vmsize),
               (unsigned)blk_size, (unsigned)blk_addr, is_text ? "RX" : "RW");

        if (alloc_block(is_text ? "ml_text" : "ml_data",
                        is_text ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RX : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                        blk_addr, blk_size) < 0) {
            fatal_error("Failed to map segment %.16s at 0x%08X", s->segname, (unsigned)blk_addr);
        }

        if (is_text) {
            text_buf = calloc(1, text_size);
            if (!text_buf)
                fatal_error("Out of memory allocating text buffer");
            if (s->filesize > 0)
                memcpy(text_buf, d + s->fileoff, s->filesize);
        } else {
            memset((void *)blk_addr, 0, blk_size);
            if (s->filesize > 0)
                memcpy((void *)(s->vmaddr + MACHO_SLIDE), d + s->fileoff, s->filesize);
        }
    }

    // 3. Scan for special sections
    for (int i = 0; i < nsects_total; i++) {
        section *s = sects[i];
        if (!strncmp(s->sectname, "__mod_init_func", 16)) {
            mod_init_addr = MACHO_SLIDE + s->addr;
            mod_init_count = s->size / 4;
            l_info("Found __mod_init_func at 0x%08X (%u initializers)", (unsigned)mod_init_addr, (unsigned)mod_init_count);
        }
    }

    // 4. Resolve dynamic symbol imports (indirect symbols and pointer stubs)
    if (dysym) {
        uint32_t *indirect = (uint32_t *)(d + dysym->indirectsymoff);
        for (int s_idx = 0; s_idx < nsects_total; s_idx++) {
            section *s = sects[s_idx];
            uint32_t type = s->flags & 0xFF;
            if (type == S_NON_LAZY_SYMBOL_POINTERS || type == S_LAZY_SYMBOL_POINTERS) {
                uint32_t nslots = s->size / 4;
                uint32_t *slots = (uint32_t *)(MACHO_SLIDE + s->addr);
                for (uint32_t i = 0; i < nslots; i++) {
                    uint32_t sym_idx = indirect[s->reserved1 + i];
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
                            if (is_data_symbol(name)) {
                                slots[i] = make_unimpl_data(name);
                            } else {
                                slots[i] = make_unimpl_thunk(name);
                            }
                        }
                    }
                }
            }
        }
    }

    // 5. Apply offline rebase relocations
    if (rebase_path)
        apply_rebase(rebase_path);

    // 6. Copy final __TEXT to executable memory block and flush CPU caches
    kuKernelCpuUnrestrictedMemcpy((void *)(MACHO_SLIDE + text_vm), text_buf, text_size);
    kuKernelFlushCaches((void *)(MACHO_SLIDE + text_vm), text_size);
    free(text_buf);
    text_buf = NULL;

    l_info("Mach-O loaded successfully.");
    return 0;
}

void macho_run_inits(void) {
    if (!mod_init_addr || !mod_init_count)
        return;
    l_info("Executing %u static C++ constructors (__mod_init_func)...", (unsigned)mod_init_count);
    uint32_t *inits = (uint32_t *)mod_init_addr;
    for (uint32_t i = 0; i < mod_init_count; i++) {
        if (inits[i]) {
            uintptr_t fn = inits[i];
            if (fn < MACHO_SLIDE)
                fn += MACHO_SLIDE;
            l_info("  [init %u/%u] invoking 0x%08X (original 0x%08X)...",
                   (unsigned)(i + 1), (unsigned)mod_init_count, (unsigned)fn, (unsigned)(fn - MACHO_SLIDE));
            call_guest((void *)fn, 0, 0, 0, 0);
        }
    }
    l_info("Static inits executed.");
}
