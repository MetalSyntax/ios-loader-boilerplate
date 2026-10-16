#!/usr/bin/env python3
"""Desensambla funciones del Mach-O (Thumb-1/ARM, vía otool) y anota los accesos PIC
(ldr rX,[pc,#n]; add rX,pc; ldr rY,[rX]) con lo que realmente cargan: selector ObjC, clase,
CFString, cadena C, import o símbolo local.

Uso: python3 -I annotate.py <Inotia2> <símbolo> [<símbolo>...]   (ej. _CS_fsOpen '-[EAGLView flushLCD]')
"""
import bisect, re, struct, subprocess, sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from macho_prep import parse  # noqa: E402

B = sys.argv[1]
d, segs, sects, (symoff, nsyms, stroff, _), dysym = parse(B)
names, syms = [], []
for i in range(nsyms):
    strx, ntype, nsect, ndesc, value = struct.unpack_from('<IBBhI', d, symoff + i * 12)
    n = d[stroff + strx:d.index(b'\0', stroff + strx)].decode('latin1')
    names.append(n)
    if not ntype & 0xE0 and (ntype & 0x0E) == 0x0E:
        syms.append((value, n))
syms.sort()
addrs = [a for a, _ in syms]
by_name = {n: a for a, n in syms}

slot_import = {}
for s in sects:
    if s['type'] in (6, 7):
        for i in range(s['size'] // 4):
            idx = struct.unpack_from('<I', d, dysym[12] + (s['r1'] + i) * 4)[0]
            if not idx & 0xC0000000:
                slot_import[s['addr'] + i * 4] = names[idx]
for i in range(dysym[15]):
    a, info = struct.unpack_from('<iI', d, dysym[14] + i * 8)
    slot_import[a] = names[info & 0xFFFFFF]


def r32(va):
    for s in segs:
        if s['vmaddr'] <= va < s['vmaddr'] + s['filesize']:
            return struct.unpack_from('<I', d, s['fileoff'] + va - s['vmaddr'])[0]
    return None


def cstr(va):
    for s in segs:
        if s['vmaddr'] <= va < s['vmaddr'] + s['filesize']:
            o = s['fileoff'] + va - s['vmaddr']
            return d[o:d.index(b'\0', o)][:60].decode('latin1')
    return '?'


def sect(va):
    return next((s['name'] for s in sects if s['addr'] <= va < s['addr'] + s['size']), None)


def describe(va):
    """Qué hay en la dirección va (destino de un ldr [rX])."""
    if va in slot_import:
        return f'&import {slot_import[va]}'
    sn = sect(va)
    v = r32(va)
    if sn == '__objc_selrefs':
        return f'@selector({cstr(v)})'
    if sn in ('__objc_classrefs', '__objc_superrefs'):
        i = bisect.bisect_right(addrs, v) - 1 if v else -1
        return f'class {syms[i][1] if i >= 0 and addrs[i] == v else hex(v or 0)}'
    if sn == '__nl_symbol_ptr':
        i = bisect.bisect_right(addrs, v) - 1
        return f'&{syms[i][1]}+{v - addrs[i]:#x}' if i >= 0 else hex(v)
    return None


def describe_addr(va):
    sn = sect(va)
    if sn == '__cfstring':
        return f'@"{cstr(r32(va + 8))}"'
    if sn == '__cstring':
        return f'"{cstr(va)}"'
    i = bisect.bisect_right(addrs, va) - 1
    return f'{syms[i][1]}+{va - addrs[i]:#x}' if i >= 0 else hex(va)


dis = subprocess.run(['otool', '-tvV', B], capture_output=True, text=True).stdout.splitlines()
want = set(sys.argv[2:])
printing, regs = False, {}
for line in dis:
    if line.endswith(':') and not line[0].isdigit():
        printing = line[:-1] in want
        regs = {}
        if printing:
            print(f'\n{line}')
        continue
    if not printing:
        continue
    m = re.match(r'([0-9a-f]{8})\s+\S+\s+(\S+)\s*(.*)', line)
    if not m:
        continue
    a, op, args = int(m.group(1), 16), m.group(2), m.group(3)
    note = ''
    mm = re.match(r'(r\d+|r12|lr), \[pc, #(0x[0-9a-f]+|\d+)\]', args)
    if op == 'ldr' and mm:
        lit = ((a + 4) & ~3) + int(mm.group(2), 0)
        regs[mm.group(1)] = ('lit', r32(lit))
    elif op == 'add' and re.match(r'(r\d+), pc$', args):
        r = args.split(',')[0]
        if regs.get(r, (None,))[0] == 'lit':
            regs[r] = ('addr', (regs[r][1] + a + 4) & 0xFFFFFFFF)
            note = describe_addr(regs[r][1])
    elif op == 'ldr' and (mm := re.match(r'(r\d+), \[(r\d+)(?:, #(0x[0-9a-f]+|\d+))?\]', args)):
        src = regs.get(mm.group(2))
        if src and src[0] == 'addr':
            note = describe(src[1] + int(mm.group(3) or '0', 0)) or ''
        regs.pop(mm.group(1), None)
    else:
        r = args.split(',')[0] if args else ''
        if r in regs and op not in ('cmp', 'str', 'strb', 'strh', 'push', 'pop', 'tst'):
            regs.pop(r, None)
    print(f'{line}{"    ; " + note if note else ""}')
