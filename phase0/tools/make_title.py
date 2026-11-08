#!/usr/bin/env python3
"""Builds a title folder (code, content, meta) for the WUP packer: make_title.py <phase0 dir> <rpx> [folder name id max_code_mb].
The XML field lists follow shinyquagsire23's homebrew titles (woominstaller, LGPL-2.1), which also supplies bootMovie.h264."""
import os
import re
import shutil
import struct
import sys

phase0, rpx = sys.argv[1:3]
folder, NAME, id_suffix, max_code_mb = (sys.argv[3:7] if len(sys.argv) > 3 else ('title', '3DS Recomp Benchmark', '334453', '64'))
title = os.path.join(phase0, folder)
TITLE_ID, GROUP_ID = '0005000010' + id_suffix, '00003344'  # 10 = normal app; 3344xx = "3D"
PUBLISHER, RPX_NAME = '3DS-WiiU Recomp', 'bench.rpx'
OS_VERSION = '000500101000400A'

for d in ('code', 'content', 'meta'):
    os.makedirs(os.path.join(title, d), exist_ok=True)

# ---- art: a blocky label on a plain background, in the exact uncompressed TGA form the menu expects
GLYPHS = {
    '3': '11110 00001 00001 01110 00001 00001 11110', 'D': '11100 10010 10001 10001 10001 10010 11100',
    'S': '01111 10000 10000 01110 00001 00001 11110', 'B': '11110 10001 10001 11110 10001 10001 11110',
    'E': '11111 10000 10000 11110 10000 10000 11111', 'N': '10001 11001 10101 10101 10011 10001 10001',
    'C': '01110 10001 10000 10000 10000 10001 01110', 'H': '10001 10001 10001 11111 10001 10001 10001',
    ' ': '00000 00000 00000 00000 00000 00000 00000',
}


def lit_pixels(w, h, text):
    scale = min(w // (len(text) * 6 + 2), h // 9)
    lit = set()
    x0, y0 = (w - (len(text) * 6 - 1) * scale) // 2, (h - 7 * scale) // 2
    for i, ch in enumerate(text):
        for row, bits in enumerate(GLYPHS[ch].split()):
            for col, bit in enumerate(bits):
                if bit == '1':
                    for dy in range(scale):
                        for dx in range(scale):
                            lit.add((x0 + (i * 6 + col) * scale + dx, y0 + row * scale + dy))
    return lit


def write_tga(name, w, h, bpp, text):
    lit = lit_pixels(w, h, text)
    bg, fg = (0x60, 0x20, 0x10), (0xFF, 0xFF, 0xFF)  # BGR: a dark blue with white text
    pixels = bytearray()
    for y in range(h - 1, -1, -1):  # bottom-up, as the sample files are
        for x in range(w):
            pixels += bytes(fg if (x, y) in lit else bg)
            if bpp == 32:
                pixels.append(0xFF)
    header = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, bpp, 8 if bpp == 32 else 0)
    footer = bytes(8) + b'TRUEVISION-XFILE.\0'
    data = header + bytes(pixels) + footer + (bytes(4) if bpp == 24 else b'')
    assert len(data) == {('iconTex', 32): 65580, ('bootLogoTex', 32): 28604, ('bootDrcTex', 24): 1229808,
                         ('bootTvTex', 24): 2764848}[(name, bpp)], name  # sizes of the known-good sample
    with open(os.path.join(title, 'meta', name + '.tga'), 'wb') as f:
        f.write(data)


write_tga('iconTex', 128, 128, 32, '3DS')
write_tga('bootLogoTex', 170, 42, 32, 'BENCH')
write_tga('bootDrcTex', 854, 480, 24, '3DS BENCH')
write_tga('bootTvTex', 1280, 720, 24, '3DS BENCH')

# ---- menu metadata: the sample's fields, this title's values
meta_path = os.path.join(title, 'meta', 'meta.xml')
with open(os.path.join(phase0, 'title', 'meta', 'meta.xml'), encoding='utf8') as f:  # first made from woominstaller's
    meta = f.read()
meta = re.sub(r'(<(?:long|short)name_\w+[^>]*>)[^<]*', lambda m: m.group(1) + NAME, meta)
meta = re.sub(r'(<publisher_\w+[^>]*>)[^<]*', lambda m: m.group(1) + PUBLISHER, meta)
for tag, value in (('product_code', 'WUP-N-B3DS'), ('title_id', TITLE_ID), ('group_id', GROUP_ID), ('os_version', OS_VERSION),
                   ('add_on_unique_id0', '00000000'), ('drc_use', '1')):
    meta, n = re.subn(r'(<%s [^>]*>)[^<]*' % tag, lambda m: m.group(1) + value, meta)
    assert n == 1, tag
assert 'Woom' not in meta and '776D7900' not in meta
with open(meta_path, 'w', encoding='utf8', newline='\n') as f:
    f.write(meta)

# ---- system metadata
APP = f'''<?xml version="1.0" encoding="utf-8"?>
<app type="complex" access="777">
  <version type="unsignedInt" length="4">16</version>
  <os_version type="hexBinary" length="8">{OS_VERSION}</os_version>
  <title_id type="hexBinary" length="8">{TITLE_ID}</title_id>
  <title_version type="hexBinary" length="2">0000</title_version>
  <sdk_version type="unsignedInt" length="4">21204</sdk_version>
  <app_type type="hexBinary" length="4">80000000</app_type>
  <group_id type="hexBinary" length="4">{GROUP_ID}</group_id>
  <os_mask type="hexBinary" length="32">{'0' * 64}</os_mask>
  <common_id type="hexBinary" length="8">0000000000000000</common_id>
</app>
'''
GROUPS = [1, 3, 9, 12, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22]  # BSP, DK, USB, UHS, FS, MCP, NIM, ACT, FPD, BOSS, ACP, PDM, AC, NDM, NSEC
PERMISSIONS = ''.join(
    f'    <p{i} type="complex">\n      <group type="unsignedInt" length="4">{g}</group>\n'
    f'      <mask type="hexBinary" length="8">FFFFFFFFFFFFFFFF</mask>\n    </p{i}>\n' for i, g in enumerate(GROUPS))
HEX4 = {'avail_size': '00000000', 'codegen_size': '00000000', 'codegen_core': '00000001', 'max_size': '40000000',
        'max_codesize': f'{int(max_code_mb) << 20:08X}', 'overlay_arena': '00000001'}
STACKS = {'default_stack0_size': '00000000', 'default_stack1_size': '00040000', 'default_stack2_size': '00000000',
          'default_redzone0_size': '00000000', 'default_redzone1_size': '00000000', 'default_redzone2_size': '00000000',
          'exception_stack0_size': '00001000', 'exception_stack1_size': '00001000', 'exception_stack2_size': '00001000'}
hex_tags = lambda d: ''.join(f'  <{k} type="hexBinary" length="4">{v}</{k}>\n' for k, v in d.items())
COS = f'''<?xml version="1.0" encoding="utf-8"?>
<app type="complex" access="777">
  <version type="unsignedInt" length="4">20</version>
  <cmdFlags type="unsignedInt" length="4">0</cmdFlags>
  <argstr type="string" length="4096">{RPX_NAME}</argstr>
{hex_tags(HEX4)}  <permissions type="complex">
{PERMISSIONS}  </permissions>
{hex_tags(STACKS)}  <num_codearea_heap_blocks type="unsignedInt" length="4">0</num_codearea_heap_blocks>
  <num_workarea_heap_blocks type="unsignedInt" length="4">0</num_workarea_heap_blocks>
</app>
'''
for name, text in (('app.xml', APP), ('cos.xml', COS)):
    with open(os.path.join(title, 'code', name), 'w', encoding='utf8', newline='\n') as f:
        f.write(text)
shutil.copyfile(rpx, os.path.join(title, 'code', RPX_NAME))
if folder != 'title':
    shutil.copyfile(os.path.join(phase0, 'title', 'meta', 'bootMovie.h264'), os.path.join(title, 'meta', 'bootMovie.h264'))

# NUSPacker requires a non-empty content folder.
with open(os.path.join(title, 'content', 'readme.txt'), 'w', newline='\n') as f:
    f.write('3DS to Wii U recomp: Phase 0 benchmark. Contains no game data.\n')

for root, _, files in os.walk(title):
    for n in sorted(files):
        p = os.path.join(root, n)
        print(f'{os.path.getsize(p):>9} {os.path.relpath(p, title)}')
