#!/usr/bin/env python3
"""Every check that needs no game and no Cemu, in a few seconds: selftest.py. Exit code 0 if all passed.
The checks of whole games are tools/check_ports.py's."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, ROOT + '/tools/win')
import build3ds  # noqa: E402  the PC's compiler and where the sources are


def vertex_arrays():
    """port/wiiu/vertex_arrays.h, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_vertex_arrays.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-w', '-D__WIIU__', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                 f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_vertex_arrays.cpp', '-o', exe], [exe]]


def main():
    py = sys.executable
    checks = {
        'vertex arrays': vertex_arrays(),
        'fragment variants': [[py, ROOT + '/tools/fs_variants.py', '--self-check']],
        'picture check': [[py, ROOT + '/tools/test_check_pictures.py']],
        'game detection': [[py, ROOT + '/tools/win/test_detect.py']],
        'shader logs': [[py, ROOT + '/builder/test_shaderlogs.py']],
        'user files': [[py, ROOT + '/builder/test_user_files.py']],
        'learned shaders': [[py, ROOT + '/builder/test_learned.py']],
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
