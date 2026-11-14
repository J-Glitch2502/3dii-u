#!/usr/bin/env python3
"""Builds and runs tools/test_gs_native.cpp for a game's logged geometry shaders: test_gs_native.py <shader_logs folder>
The native code is made from those logs here (tools/pica_to_c.py) and compared with the PICA interpreter, as the shader generator's own
objects build it (build3ds.build_shadergen). Nothing it makes is kept in the repository: the logs are the game's code."""
import contextlib
import glob
import io
import os
import subprocess
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'win'))
import build3ds  # noqa: E402


def main():
    gsl = sorted(glob.glob(os.path.join(sys.argv[1], '*.gsl')))
    if not gsl:
        sys.exit('no .gsl logs there')
    with contextlib.redirect_stdout(io.StringIO()):
        build3ds.build_shadergen(types.SimpleNamespace(jobs=4, force=False, extra_flags=None, only=None, opt=None))
    out = build3ds.WORK + '/selftest'
    os.makedirs(out, exist_ok=True)
    made = out + '/gs_native.c'
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'pica_to_c.py'), made] + gsl, check=True)
    objs = [f'{build3ds.WORK}/shadergen/{n}.o' for n in ('shader.cpp', 'shadergen_stubs.cpp')]
    cc = [c.replace('g++', 'gcc') if os.path.basename(c).startswith('g++') else c for c in build3ds.HOST_CXX]
    subprocess.run(cc + ['-O1', '-c', made, '-o', out + '/gs_native.o'], check=True)
    exe = out + '/test_gs_native.exe'
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc',
                                       f'-I{build3ds.RT}', f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static',
                                       os.path.join(ROOT, 'tools', 'test_gs_native.cpp'), out + '/gs_native.o', *objs, '-lpthread', '-o', exe], check=True)
    sys.exit(subprocess.run([exe] + gsl).returncode)


if __name__ == '__main__':
    main()
