#!/usr/bin/env python3
"""Runs an RPX through the checks the Wii U's loader makes, as far as they can be made off the console (decaf-emu's reimplementation is the
reference). Cemu accepts files the console refuses. rpx_check.py <file.rpx> <cos.xml>"""
import re
import struct
import sys
import zlib

data = open(sys.argv[1], 'rb').read()
cos = open(sys.argv[2]).read()
problems = []


def bad(msg):
    problems.append(msg)
    print('FAIL', msg)


assert data[:4] == b'\x7fELF'
shoff = struct.unpack('>I', data[0x20:0x24])[0]
shentsize, shnum, shstrndx = struct.unpack('>HHH', data[0x2E:0x34])
FIELDS = ('name', 'type', 'flags', 'addr', 'offset', 'size', 'link', 'info', 'addralign', 'entsize')
secs = [dict(zip(FIELDS, struct.unpack('>10I', data[shoff + i * shentsize:shoff + i * shentsize + 40]))) for i in range(shnum)]
SHF_WRITE, SHF_ALLOC, SHF_EXEC, SHF_DEFLATED = 1, 2, 4, 0x08000000
EXPORTS, IMPORTS, CRCS, FILEINFO, NOBITS = 0x80000001, 0x80000002, 0x80000003, 0x80000004, 8


def body(s):
    raw = data[s['offset']:s['offset'] + s['size']]
    if s['flags'] & SHF_DEFLATED:
        out = zlib.decompress(raw[4:])
        if len(out) != struct.unpack('>I', raw[:4])[0]:
            bad('inflated size differs from its header')
        return out
    return raw


names = body(secs[shstrndx])
for s in secs:
    s['n'] = names[s['name']:names.index(b'\0', s['name'])].decode()

CHUNK = 0x400000  # the first read
if shoff >= CHUNK or (shnum - 1) * shentsize + shoff >= CHUNK:
    bad('section headers past the first chunk')
crc, info = secs[shnum - 2], secs[shnum - 1]
if crc['type'] != CRCS or crc['flags'] & SHF_DEFLATED:
    bad('CRC section wrong')
if info['type'] != FILEINFO or info['flags'] & SHF_DEFLATED:
    bad('file info section wrong')
if crc['offset'] + crc['size'] >= CHUNK or info['offset'] + info['size'] >= CHUNK:
    bad('CRC or file info past the first chunk')

crcs = struct.unpack('>%dI' % (crc['size'] // 4), body(crc))
for i, s in enumerate(secs):
    if s['type'] == NOBITS or s['size'] == 0 or i == shnum - 2:
        continue
    got = zlib.crc32(body(s)) & 0xffffffff
    if got != crcs[i]:
        bad('CRC differs for section %d %s: %08x vs %08x' % (i, s['n'], got, crcs[i]))
b = body(info)
keys = 'magic textSize textAlign dataSize dataAlign loadSize loadAlign tempSize trampAdjust sdaBase sda2Base stackSize filename flags heapSize tagOffset minVersion compressionLevel trampAddition fileInfoPad cafeSdkVersion cafeSdkRevision'.split()
fi = dict(zip(keys, struct.unpack('>%dI' % len(keys), b[:4 * len(keys)])))
if not 20500 <= fi['minVersion'] <= 21301:
    bad('SDK version %d out of range' % fi['minVersion'])
if fi['magic'] < 0xCAFE0402:
    bad('file info version too old')

# file order: data, read-only, text, temp (LiCheckFileBounds)
groups = {'data': [], 'read': [], 'text': [], 'temp': []}
for s in secs[1:]:
    if s['size'] == 0 or s['type'] in (FILEINFO, IMPORTS, CRCS, NOBITS):
        continue
    if s['flags'] & SHF_EXEC and s['type'] != EXPORTS:
        g = 'text'
    elif s['flags'] & SHF_ALLOC and s['flags'] & SHF_WRITE:
        g = 'data'
    elif s['flags'] & SHF_ALLOC:
        g = 'read'
    else:
        g = 'temp'
    groups[g].append((s['offset'], s['offset'] + s['size'], s['n']))
order = ['data', 'read', 'text', 'temp']
prev_max = 0
for g in order:
    if groups[g]:
        lo, hi = min(x[0] for x in groups[g]), max(x[1] for x in groups[g])
        print('  file %-5s %#10x .. %#10x' % (g, lo, hi))
        if lo < prev_max:
            bad('%s starts before the previous group ends' % g)
        prev_max = hi

# segments
seg = {'text': [], 'data': [], 'load': []}
for s in secs[1:]:
    if not s['flags'] & SHF_ALLOC or s['size'] == 0 or s['type'] in (FILEINFO, CRCS):
        continue
    size = struct.unpack('>I', data[s['offset']:s['offset'] + 4])[0] if s['flags'] & SHF_DEFLATED else s['size']
    if s['flags'] & SHF_EXEC and s['type'] not in (EXPORTS, IMPORTS):
        k = 'text'
    elif s['flags'] & SHF_WRITE:
        k = 'data'
    else:
        k = 'load'
    seg[k].append((s['addr'], s['addr'] + size, s['addralign'], s['n']))
    if s['addralign'] > 1 and s['addr'] % s['addralign']:
        bad('section %s misaligned' % s['n'])
for k, size in (('text', fi['textSize'] - fi['trampAdjust']), ('data', fi['dataSize']), ('load', fi['loadSize'] - fi['fileInfoPad'])):
    lo, hi = min(x[0] for x in seg[k]), max(x[1] for x in seg[k])
    print('  %-5s %#x .. %#x, needs %#x, file info gives %#x' % (k, lo, hi, hi - lo, size))
    if hi - lo > size:
        bad('%s segment bigger than its file info size' % k)

# memory
maxcode = int(re.search(r'<max_codesize[^>]*>([0-9A-Fa-f]+)<', cos).group(1), 16)
maxsize = int(re.search(r'<max_size[^>]*>([0-9A-Fa-f]+)<', cos).group(1), 16)
temp = sum(s['size'] for s in secs[1:] if not s['flags'] & SHF_ALLOC and s['type'] not in (FILEINFO, CRCS))
print('  code heap %#x (%d MB): text %#x + compressed relocations %#x, left %.1f MB; code would start at %#x' % (
    maxcode, maxcode >> 20, fi['textSize'], temp, (maxcode - fi['textSize'] - temp) / 1048576, 0x10000000 - maxcode))
if fi['textSize'] + temp > maxcode:
    bad('text and relocations do not fit the code heap')
if maxcode > 0x0E000000:
    bad('max_codesize over the limit')

# relocations
sym = next(s for s in secs if s['type'] == 2)
syms = body(sym)
kinds = {}
GOOD = {0, 1, 4, 5, 6, 10, 11, 68, 73, 109, 251, 252, 253}  # NONE ADDR32 ADDR16_LO/HI/HA REL24 REL14 DTPMOD32 DTPREL32 EMB_SDA21 GHS_REL16_HA/HI/LO
text_lo = min(x[0] for x in seg['text'])
text_base = 0x10000000 - maxcode
for s in secs[1:]:
    if s['type'] != 4:
        continue
    r = body(s)
    if len(r) % 12:
        bad('%s size is not a multiple of 12' % s['n'])
    for o in range(0, len(r), 12):
        off, inf, add = struct.unpack('>IIi', r[o:o + 12])
        t = inf & 255
        kinds[t] = kinds.get(t, 0) + 1
        if t not in GOOD:
            bad('unsupported relocation type %d in %s' % (t, s['n']))
            break
        si = inf >> 8
        if si >= len(syms) // 16:
            bad('symbol index out of range in %s' % s['n'])
            break
        if t in (10, 11):
            nm, val, size, inf2, other, shndx = struct.unpack('>IIIBBH', syms[si * 16:si * 16 + 16])
            print('    branch reloc type %d at %#x, symbol section %d value %#x bind %d' % (t, off, shndx, val, inf2 >> 4))
print('  relocation types:', dict(sorted(kinds.items())))
print('  entry %#x' % struct.unpack('>I', data[0x18:0x1C])[0])
print('RESULT:', 'no problems found' if not problems else '%d problems' % len(problems))
