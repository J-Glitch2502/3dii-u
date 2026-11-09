#!/usr/bin/env python3
"""Checks 3Dii U builder dependencies on Windows and Linux: python tools/setup.py [--check]

Checks every tool the build needs, installs what it can, and says how to get the rest. Run it again after installing
anything: it skips what's already there. --check only reports, and changes nothing.

Maintainers: --pack writes the dependencies zip from this PC's copies, for a new release (DEPS_TAG below)."""
import argparse
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MSYS2 = 'C:/msys64'
LLVM = 'C:/Program Files/LLVM'
DEVKITPRO = 'C:/devkitPro/opt/devkitpro'
TOOLCHAIN = ['devkitppc-gcc', 'devkitppc-binutils', 'devkitppc-newlib', 'devkitppc-crtls', 'wut', 'wut-tools', 'general-tools']
PIP = {'PySide6': 'PySide6-Essentials', 'pyctr': 'pyctr', 'Crypto': 'pycryptodome', 'cryptography': 'cryptography'}

# Binaries the build needs that no package manager has. They come as one zip attached to a release of this repository,
# checked against this hash before anything is unpacked.
DEPS_TAG = 'deps-1'
DEPS_ZIP = '3dii-u-deps-1.zip'
DEPS_SHA256 = '9cad1a44cfd2322cbc1c269e6774adb136cf57c5b1eb2d0273a4fa1c0a9caa0e'
DEPS = ['third_party/cafeglsl/glslcompiler.exe', 'third_party/cafeglsl/libwinpthread-1.dll', 'third_party/cafeglsl/zlib1.dll',
        'third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc', 'toolchain/aac_ppc', 'builder/bin/makerom.exe']
NOTICE = 'third_party/DEPS-NOTICES.txt'
NOTICES = """Build dependencies for 3Dii U, unpacked by tools/setup.py. Each is under its own licence.

third_party/cafeglsl/glslcompiler.exe: CafeGLSL v0.2.0 by Exzap (MIT), built for Windows by tools/win/build_cafeglsl.ps1.
  https://github.com/Exzap/CafeGLSL
third_party/cafeglsl/libwinpthread-1.dll and zlib1.dll: MinGW-w64 winpthreads (MIT and BSD-style) and zlib (zlib licence),
  from MSYS2. https://www.mingw-w64.org/ and https://zlib.net/
third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc: CafeGLSL v1.0.0-rc1's Wii U library (MIT, licence text inside), with its
  five trap instructions made no-ops by tools/win/patch_cafeglsl_traps.py.
toolchain/aac_ppc: FFmpeg 4.4's libavcodec and libavutil with only the AAC decoder (LGPL-2.1 or later), built by
  tools/aac_build.sh. Source: https://ffmpeg.org/download.html
builder/bin/makerom.exe: makerom v0.19.0 from Project_CTR by 3DSGuy (MIT). https://github.com/3DSGuy/Project_CTR
"""


def run(argv, **kw):
    print('  >', ' '.join(argv), flush=True)
    return subprocess.run(argv, **kw).returncode == 0


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def pip_missing():
    return [pkg for mod, pkg in PIP.items() if importlib.util.find_spec(mod) is None]


def deps_missing():
    return [p for p in DEPS if not os.path.exists(os.path.join(ROOT, p))]


def install_mingw():
    return run([MSYS2 + '/usr/bin/bash.exe', '-lc', 'pacman -S --needed --noconfirm mingw-w64-x86_64-gcc'])


def install_toolchain():
    return run([MSYS2 + '/usr/bin/bash.exe', '--noprofile', '--norc', 'tools/win/install_toolchain.sh'] + TOOLCHAIN, cwd=ROOT)


def fetch_deps():
    if not shutil.which('gh') or subprocess.run(['gh', 'auth', 'status'], capture_output=True).returncode:
        print('  needs the GitHub CLI, signed in: winget install GitHub.cli, then gh auth login')
        return False
    with tempfile.TemporaryDirectory() as tmp:
        if not run(['gh', 'release', 'download', DEPS_TAG, '--pattern', DEPS_ZIP, '--dir', tmp], cwd=ROOT):
            return False
        path = os.path.join(tmp, DEPS_ZIP)
        if sha256(path) != DEPS_SHA256:
            print(f'  {DEPS_ZIP} is not the file this script expects (SHA-256 {sha256(path)}), so nothing was unpacked')
            return False
        with zipfile.ZipFile(path) as z:
            z.extractall(ROOT)
    return True


CHECKS = [  # name, test, what installs it (or None), what to do by hand
    ('Python packages', lambda: not pip_missing(), lambda: run([sys.executable, '-m', 'pip', 'install', *pip_missing()]),
     'python -m pip install ' + ' '.join(PIP.values())),
    ('MSYS2', lambda: os.path.exists(MSYS2 + '/usr/bin/bash.exe'), None, 'install it from https://www.msys2.org/ into C:\\msys64'),
    ('MinGW-w64 GCC', lambda: os.path.exists(MSYS2 + '/mingw64/bin/gcc.exe'), install_mingw,
     'in an MSYS2 shell: pacman -S mingw-w64-x86_64-gcc'),
    ('LLVM', lambda: os.path.exists(LLVM + '/bin/clang.exe'), None, 'winget install LLVM.LLVM'),
    ('devkitPPC and wut', lambda: os.path.exists(DEVKITPRO + '/devkitPPC/bin/powerpc-eabi-gcc.exe'), install_toolchain,
     'needs MSYS2 first; see CONTRIBUTING.md'),
    ('CafeGLSL, the AAC decoder and makerom', lambda: not deps_missing(), fetch_deps,
     f'gh release download {DEPS_TAG} --pattern {DEPS_ZIP}, then unzip it into the repository'),
]


def pack():
    if deps_missing():
        sys.exit('not on this PC: ' + ', '.join(deps_missing()))
    out = os.path.join(ROOT, DEPS_ZIP)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(NOTICE, NOTICES)
        for p in DEPS:
            full = os.path.join(ROOT, p)
            files = [full] if os.path.isfile(full) else [os.path.join(r, f) for r, _, fs in os.walk(full) for f in fs]
            for f in sorted(files):
                z.write(f, os.path.relpath(f, ROOT).replace('\\', '/'))
    print(f'{out}\nSHA-256 {sha256(out)}\nPut that hash in DEPS_SHA256, then upload the zip:\n'
          f'  gh release create {DEPS_TAG} "{out}" --title "Build dependencies" --notes "Unpacked by tools/setup.py"')


def linux_setup(check):
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'win'))
    import build3ds

    found = []
    if not shutil.which('clang++') and not shutil.which('g++'):
        found.append('C++ compiler: install clang++ or g++')
    if not pip_missing():
        print('ok       Python packages')
    else:
        found.append('Python packages: ' + ' '.join(pip_missing()))
        print('MISSING  Python packages: ' + ' '.join(pip_missing()))
    missing = build3ds.missing_tools(code=True, shaders=True)
    for item in missing:
        print('MISSING  ' + item)
        found.append(item)
    makerom = os.path.join(ROOT, 'builder', 'bin', 'makerom')
    if not (shutil.which('makerom') or os.path.isfile(makerom)):
        print('OPTIONAL native makerom missing: encrypted base-game CIA conversion needs it')
    if not (os.path.isfile(ROOT + '/toolchain/aac_ppc/lib/libavcodec.a') and
            os.path.isfile(ROOT + '/toolchain/aac_ppc/lib/libavutil.a')):
        print('OPTIONAL AAC decoder libraries missing: untested and AAC-using games need them')
    if found:
        print('\nInstall dependencies listed in builder/README.md. Use tools/linux/build_cafeglsl.sh for the native shader compiler.')
        return 1
    print('\nLinux build tools ready. Run python builder/app.py or python builder/pipeline.py --help.')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true', help='only report, change nothing')
    ap.add_argument('--pack', action='store_true', help='maintainers: write the dependencies zip from this PC')
    a = ap.parse_args()
    if os.name != 'nt':
        if a.pack:
            sys.exit('--pack produces the Windows dependency archive. Use Windows for release packaging.')
        return linux_setup(a.check)
    if sys.version_info < (3, 10):
        sys.exit('Python 3.10 or newer is needed: https://www.python.org/')
    if a.pack:
        return pack()
    todo = []
    for name, ok, install, by_hand in CHECKS:
        if not ok() and install and not a.check:
            print(f'{name}: installing', flush=True)
            install()
        good = ok()
        print(f'{"ok     " if good else "MISSING"}  {name}', flush=True)
        if not good:
            todo.append(f'  {name}: {by_hand}')
    if todo:
        print('\nStill to do, then run this again:\n' + '\n'.join(todo))
        return 1
    print('\nReady. Try python builder/selftest.py, then python builder/pipeline.py --help')
    return 0


if __name__ == '__main__':
    sys.exit(main())
