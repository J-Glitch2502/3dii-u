"""Shader logs without the games' code, for the standalone app.

A run's shader log (port/pc/vs_log.cpp, or the console's shader_misses.bin) holds "VS  " records, each with the GPU's whole vertex shader memory,
which is the game's own code, and fragment records (render state keys), which aren't. The app ships the fragment records as they are and each
vertex record as a fingerprint of the program of the game's DVLB its memory starts with (tools/dvlb_to_log.py finds those in a dump), in a .vsf
file beside the log. The build finds the same programs in the user's own dump and puts the records back together: the same entry, uniforms and
output map, so the shader generator makes the same shader. A vertex shader the console lacks is drawn the CPU way, so a record left out costs
speed in that scene, never the picture."""
import hashlib
import os
import struct

TAIL = 56  # bool uniforms, int uniforms, output map
VSF = 4 + 4 + 16 + TAIL  # b'VSF ', entry, fingerprint, tail


def records(d):
    """('vs', entry, code, swizzles, tail) or ('fs', raw bytes) for each record of a log, as tools/shadergen_main.cpp reads them"""
    p = 0
    while p + 8 <= len(d):
        if d[p:p + 4] == b'VS  ':
            entry, nc, ns = struct.unpack_from('<III', d, p + 4)
            q = p + 16 + (nc + ns) * 4
            yield 'vs', entry, d[p + 16:p + 16 + nc * 4], d[p + 16 + nc * 4:q], d[q:q + TAIL]
            p = q + TAIL
        else:
            q = p + 4 if d[p:p + 4] == b'FS  ' else p
            if q + 4 > len(d):
                break
            end = q + 4 + struct.unpack_from('<I', d, q)[0]
            yield 'fs', d[p:end]
            p = end


def programs(dvlb):
    """the game's vertex programs as (code, swizzles), from dvlb_to_log.py's dvlb.bin"""
    with open(dvlb, 'rb') as f:
        return {(r[2], r[3]) for r in records(f.read()) if r[0] == 'vs'}


def fingerprint(code, swz):
    return hashlib.sha256(code + b'|' + swz).digest()[:16]


def split(d, progs):
    """(fragment records as they are, vertex records as fingerprints, how many were left out). A record whose shader memory starts with none of
    the game's programs has nothing to be found by, so it's left out."""
    frag, vsf, left = bytearray(), bytearray(), 0
    for r in records(d):
        if r[0] == 'fs':
            frag += r[1]
            continue
        _, entry, code, swz, tail = r
        found = [p for p in progs if entry < len(p[0]) // 4 and code.startswith(p[0]) and swz.startswith(p[1])]
        if found:
            vsf += b'VSF ' + struct.pack('<I', entry) + fingerprint(*max(found, key=lambda p: len(p[0]))) + tail
        else:
            left += 1
    return bytes(frag), bytes(vsf), left


def restore(logs):
    """Each .vsf file in a game's shader_logs, with the programs of this dump's dvlb.bin, back into "VS  " records (<name>_vsf.bin). A fingerprint
    this dump doesn't have (another version of the game) is left out. Returns (records made, left out)."""
    names = [f for f in os.listdir(logs) if f.endswith('.vsf')]
    dvlb = os.path.join(logs, 'dvlb.bin')
    found = {fingerprint(c, s): (c, s) for c, s in programs(dvlb)} if names and os.path.isfile(dvlb) else {}
    made = left = 0
    for name in names:
        with open(os.path.join(logs, name), 'rb') as f:
            d = f.read()
        out = bytearray()
        for p in range(0, len(d) - VSF + 1, VSF):
            if d[p:p + 4] != b'VSF ':
                break
            prog = found.get(d[p + 8:p + 24])
            if prog is None:
                left += 1
                continue
            code, swz = prog
            out += b'VS  ' + struct.pack('<III', struct.unpack_from('<I', d, p + 4)[0], len(code) // 4, len(swz) // 4) + code + swz + d[p + 24:p + VSF]
            made += 1
        with open(os.path.join(logs, name[:-4] + '_vsf.bin'), 'wb') as f:
            f.write(out)
        os.remove(os.path.join(logs, name))
    return made, left
