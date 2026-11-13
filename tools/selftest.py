#!/usr/bin/env python3
"""Every check that needs no game and no Cemu, in a few seconds: selftest.py. Exit code 0 if all passed.
The checks of whole games are tools/check_ports.py's."""
import contextlib
import io
import os
import subprocess
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, ROOT + '/tools/win')
import build3ds  # noqa: E402  the PC's compiler and where the sources are


def vertex_arrays():
    """port/wiiu/vertex_arrays.h, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_vertex_arrays.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-w', '-D__WIIU__', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                 f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_vertex_arrays.cpp', '-o', exe], [exe]]


def present_rule():
    """port/wiiu/present_rule.h against made-up frame times, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_present_rule.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_present_rule.cpp', '-lpthread', '-o', exe], [exe]]


def frame_cap():
    """port/wiiu/frame_cap.h against made-up games, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_frame_cap.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_frame_cap.cpp', '-lpthread', '-o', exe], [exe]]


def latte_program():
    """port/wiiu/latte_program.h against small programs made up word by word, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_latte_program.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_latte_program.cpp', '-lpthread', '-o', exe], [exe]]


def open_bools():
    """tools/test_open_bools.cpp with the shader generator's own objects, which build3ds keeps built, for this PC"""
    with contextlib.redirect_stdout(io.StringIO()):
        build3ds.build_shadergen(types.SimpleNamespace(jobs=4, force=False, extra_flags=None, only=None, opt=None))
    exe = build3ds.WORK + '/selftest/test_open_bools.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    objs = [f'{build3ds.WORK}/shadergen/{n}.o' for n in ('shadergen.cpp', 'shaderkeys.cpp', 'shader.cpp', 'shadergen_stubs.cpp')]
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                 f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_open_bools.cpp', *objs,
                                 '-lpthread', '-o', exe], [exe]]


def main():
    py = sys.executable
    checks = {
        'vertex arrays': vertex_arrays(),
        'present rule': present_rule(),
        'frame cap': frame_cap(),
        'compiled programs': latte_program(),
        'open bools': open_bools(),
        'program rules': [[py, ROOT + '/tools/gsh_rules.py', '--self-check']],
        'fragment variants': [[py, ROOT + '/tools/fs_variants.py', '--self-check']],
        'picture check': [[py, ROOT + '/tools/test_check_pictures.py']],
        'WUP packer': [[py, ROOT + '/tools/test_pack_wup.py']],
        'game detection': [[py, ROOT + '/tools/win/test_detect.py']],
        'shader logs': [[py, ROOT + '/builder/test_shaderlogs.py']],
        'user files': [[py, ROOT + '/builder/test_user_files.py']],
        'system archives': [[py, ROOT + '/builder/test_system_archives.py']],
        'builder app': [[py, ROOT + '/builder/test_app.py']],
        'learned shaders': [[py, ROOT + '/builder/test_learned.py']],
        'DLC reader': [[py, ROOT + '/builder/selftest.py']],
    }
    failed = []
    for name, commands in checks.items():
        for argv in commands:
            r = subprocess.run(argv, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
            if r.returncode:
                failed.append(name)
                print(f'FAILED {name}:\n{r.stdout[-1500:]}')
                break
        else:
            print(f'ok     {name}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
