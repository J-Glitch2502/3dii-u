#!/usr/bin/env python3
"""Builds the 3DS-to-Wii-U ports on Windows or Linux. Run with Python from anywhere:

    build3ds.py code <game>            runtime, game code and link: the RPX and game.nm
    build3ds.py shaders <game>         the shader pack
    build3ds.py title <game>           the title folder, for Cemu and the WUP packer
    build3ds.py all <game>             those three
    build3ds.py pack <game> <otp.bin>  the WUP folder, in dist/
    build3ds.py gen <game>             the game's ARM code to C again
    build3ds.py doctor [game]          check the installed toolchain and shader compilers
    build3ds.py trial <game> [frames]  Windows only: run the headless game on this PC

Options: --jobs N (default 4), --force, --extra-flags "...", --only a.cpp,b.cpp (recompile only these runtime files), --opt -O1, --out <folder> (keep a test build apart).

Compiles run at below-normal priority on Windows and with bounded parallelism on Linux. Objects live in build_win/ or
build_linux/ respectively. The Wii U RPX, shader pack, title and WUP outputs keep their usual locations.
Linux needs native devkitPPC/wut, the Wii U CafeGLSL library and a CafeGLSL v0.2.0 shader compiler; use `doctor` to check.
The `trial` and `lazy` developer commands currently require Windows.
"""
import argparse
import ctypes
import glob
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace('\\', '/')
WINDOWS = sys.platform == 'win32'
WORK = ROOT + ('/build_win' if WINDOWS else '/build_linux')
RT = ROOT + '/third_party/3dsrecomp/runtime'
PORT = ROOT + '/port'
DKP = (os.environ.get('THREEDII_DEVKITPRO') or os.environ.get('DEVKITPRO') or
       ('C:/devkitPro/opt/devkitpro' if WINDOWS else '/opt/devkitpro')).rstrip('/\\')
BIN = DKP + '/devkitPPC/bin/powerpc-eabi-'
WUT = DKP + '/wut'
TOOL_EXT = '.exe' if WINDOWS else ''
LLVM = 'C:/Program Files/LLVM/bin/' if WINDOWS else ''
MINGW = 'C:/msys64/mingw64'
GLSLC = (os.environ.get('THREEDII_GLSLC') or
         (ROOT + '/third_party/cafeglsl/glslcompiler.exe' if WINDOWS else
          next((path for path in (
              ROOT + '/third_party/cafeglsl/glslcompiler.elf',
              ROOT + '/third_party/cafeglsl/glslcompiler',
              ROOT + '/third_party/cafeglsl/build/cafecompiler/glslcompiler.elf',
              shutil.which('glslcompiler.elf'), shutil.which('glslcompiler')) if path and os.path.isfile(path)),
              ROOT + '/third_party/cafeglsl/glslcompiler.elf')))
CAFEGLSL_PPC = os.environ.get('THREEDII_CAFEGLSL_PPC') or ROOT + '/third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc'
HOST_CXX = ([LLVM + 'clang++.exe', '--target=x86_64-w64-mingw32', '--sysroot=' + MINGW, '-fuse-ld=lld']
            if WINDOWS else [shutil.which('clang++') or shutil.which('g++') or 'c++'])
TOMODACHI_ID = '000400000008c300'
BELOW_NORMAL = 0x4000
MIN_FREE_MB = 2500

os.environ['DEVKITPRO'] = DKP
os.environ['DEVKITPPC'] = DKP + '/devkitPPC'
os.environ['PATH'] = os.pathsep.join([DKP + '/devkitPPC/bin', DKP + '/tools/bin', os.environ.get('PATH', '')])


class MemoryStatus(ctypes.Structure):
    _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong), ('avail', ctypes.c_ulonglong),
                ('total_page', ctypes.c_ulonglong), ('avail_page', ctypes.c_ulonglong), ('total_virtual', ctypes.c_ulonglong),
                ('avail_virtual', ctypes.c_ulonglong), ('avail_extended', ctypes.c_ulonglong)]


def free_mb():
    if WINDOWS:
        m = MemoryStatus()
        m.length = ctypes.sizeof(m)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.avail >> 20
    try:
        with open('/proc/meminfo') as f:
            for line in f:
                if line.startswith('MemAvailable:'):
                    return int(line.split()[1]) >> 10
    except OSError:
        pass
    return MIN_FREE_MB + 1


def run(argv, **kw):
    return subprocess.run(argv, **({'creationflags': BELOW_NORMAL} if WINDOWS else {}), **kw)


def tool(name):
    return BIN + name + TOOL_EXT


def run_jobs(jobs, n, what, timeout=None, fallback=None):
    """jobs: (label, argv, cwd or None, called on success or None). Returns the labels that failed. Waits for free memory before starting each, unless nothing else is running.
    A job taking longer than timeout seconds is run again with the fallback flags added."""
    if not jobs:
        return []
    lock = threading.Lock()
    state = {'running': 0, 'done': 0}
    failed = []
    t0 = time.time()

    def one(job):
        label, argv, cwd, after = job
        while True:
            with lock:
                if state['running'] == 0 or free_mb() > MIN_FREE_MB:
                    state['running'] += 1
                    break
            time.sleep(2)
        try:
            r = run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=timeout)
        except subprocess.TimeoutExpired:
            print(f'  {label}: over {timeout} s, trying {" ".join(fallback)}', flush=True)
            r = run(argv + fallback, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        with lock:
            state['running'] -= 1
            state['done'] += 1
            if r.returncode:
                failed.append(label)
                print(f'=== {label}')
                for line in [x for x in r.stdout.splitlines() if re.search(r'error|Error|undefined', x)][:8] or r.stdout.splitlines()[:8]:
                    print(line[:190])
            elif state['done'] % max(1, len(jobs) // 10) == 0:
                print(f'  {what}: {state["done"]} of {len(jobs)}, {time.time() - t0:.0f} s', flush=True)
        if not r.returncode and after:
            after(r)

    with ThreadPoolExecutor(n) as pool:
        list(pool.map(one, jobs))
    print(f'{what}: {len(jobs)} in {time.time() - t0:.0f} s' + (f', {len(failed)} failed' if failed else ''), flush=True)
    return failed


def read_deps(path):
    if not os.path.exists(path):
        return None
    text = open(path, errors='replace').read().replace('\\\n', ' ')
    text = text.split(':', 2)[-1] if text[1:2] == ':' else text.split(': ', 1)[-1]
    return [d.replace('\\ ', ' ') for d in re.findall(r'(?:\\ |\S)+', text)]


def stale(obj, src, argv, force):
    """Whether to compile: asked to, or no object, or its command changed, or the source or any header it read is newer."""
    if force or not os.path.exists(obj):
        return True
    cmd = obj + '.cmd'
    if not os.path.exists(cmd) or open(cmd).read() != ' '.join(argv):
        return True
    when = os.path.getmtime(obj)
    deps = read_deps(obj + '.d')
    if deps is None:
        return os.path.getmtime(src) > when
    return any(not os.path.exists(d) or os.path.getmtime(d) > when for d in deps + [src])


def compile_all(items, n, what, force=False, timeout=None, fallback=None):
    """items: (label, source, object, argv without the output or dependency file)"""
    jobs = []
    for label, src, obj, argv in items:
        if stale(obj, src, argv, force):
            def remember(r, obj=obj, argv=argv):
                open(obj + '.cmd', 'w').write(' '.join(argv))
            jobs.append((label, argv + ['-c', src, '-o', obj, '-MMD', '-MF', obj + '.d'], None, remember))
    print(f'{what}: {len(items) - len(jobs)} of {len(items)} up to date', flush=True)
    if run_jobs(jobs, n, what, timeout, fallback):
        sys.exit(f'{what}: compile errors above')


class Game:
    def __init__(self, name):
        self.name = name
        self.dir = f'{ROOT}/games/{name}'
        self.cro_base = None
        self.keep_gen = False
        self.no_uber = False
        self.pipelines = None
        self.extra_functions = f'{self.dir}/functions.txt'
        self.opt = []
        # the decoder behind the DSP's AAC pipe; without it those tracks are silent. Games nobody has checked (the builder's) get it: it costs only size
        self.aac = name not in ('sticker_star', 'nsmb2', 'dream_team', 'paper_jam', 'layton_azran', 'layton_mask', 'tomodachi')
        if name == 'tomodachi':
            self.family, self.gen, self.mode = 'tomo', 'gen_abi', 'abi'
            self.out, self.rpx, self.nm, self.shaders = ROOT + '/build', ROOT + '/build/tomodachi.rpx', ROOT + '/build/game.nm', ROOT + '/build/shaders'
            self.keep_gen = self.no_uber = True
            self.extra_functions = f'{ROOT}/third_party/3dsrecomp/games/{TOMODACHI_ID}/functions.txt'
            self.pipelines = f'{ROOT}/third_party/3dsrecomp/games/{TOMODACHI_ID}/pipelines.bin'
        elif name.startswith('layton_'):
            self.family, self.gen, self.mode = 'layton', 'gen_safe', 'safe'
            self.out = f'{ROOT}/build_layton/{name}'
            self.keep_gen = True
            # -O1: at -O2 gcc reuses a compare across a jump, which the console runs and Cemu's recompiler gets wrong: see tools/layton_wiiu.sh
            self.opt = ['-O1']
        else:
            self.family, self.gen, self.mode = 'mario', 'gen_abi', 'abi'
            self.out = f'{ROOT}/build_mario/{name}'
            self.cro_base = '0x1C000000'
        if self.family != 'tomo':
            self.rpx, self.nm, self.shaders = f'{self.out}/{name}.rpx', f'{self.out}/game.nm', f'{self.out}/shaders'
        self.gen_dir = f'{self.dir}/{self.gen}'


# ---------------------------------------------------------------- recompiling the game's ARM code to C
def need_gen(g):
    table = f'{g.gen_dir}/table.c'
    if not os.path.exists(table):
        return True
    if g.keep_gen:
        return False
    t = os.path.getmtime(table)
    tools = [f'{ROOT}/third_party/3dsrecomp/tools/recomp/{f}.py' for f in ('gen', 'cro')]
    return any(os.path.exists(f) and os.path.getmtime(f) > t for f in tools + [g.extra_functions])


def game_env(g):
    env = dict(os.environ)
    if g.cro_base:
        env['RECOMP_CRO_BASE'] = g.cro_base
    return env


def do_gen(g, args, gen_dir=None, abi=None):
    argv = [sys.executable, ROOT + '/tools/win/gen_lf.py', g.dir, gen_dir or g.gen_dir, '--extra', g.extra_functions]
    if g.mode == 'abi' if abi is None else abi:
        argv.append('--abi')
    r = run(argv, cwd=ROOT + '/third_party/3dsrecomp', env=game_env(g), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    for line in r.stdout.splitlines():
        if re.search(r'instructions:|^functions|undefined instructions|Error|Traceback', line):
            print(line[:140])
    if r.returncode:
        sys.exit(r.stdout[-2000:])


# ---------------------------------------------------------------- the Wii U build
def link_setup():
    """wut's linker files adjusted for very large code: see tools/rpx_link_setup.sh"""
    d = WORK + '/rpx'
    os.makedirs(d, exist_ok=True)
    s = open(WUT + '/share/wut.ld').read()
    init, fini = '      KEEP (*(SORT_NONE(.init)))\n', '      KEEP (*(SORT_NONE(.fini)))\n'
    assert init in s and fini in s
    s = s.replace(fini, '').replace(init, init + fini + '      *crtbegin*.o(.text .text.*)\n      *crtend*.o(.text .text.*)\n')
    open(d + '/rpx.ld', 'w', newline='\n').write(s)
    spec = open(WUT + '/share/wut.specs').read()
    assert '-T %:getenv(DEVKITPRO /wut/share/wut.ld)' in spec
    open(d + '/rpx.specs', 'w', newline='\n').write(spec.replace('-T %:getenv(DEVKITPRO /wut/share/wut.ld)', '-T rpx/rpx.ld'))


def runtime_flags(extra):
    return ['-std=c++20', '-O2', '-g0', '-w', '-mcpu=750', '-meabi', '-mhard-float', '-ffunction-sections', '-fno-strict-aliasing', '-fno-exceptions', '-fno-if-conversion',
            '-fno-if-conversion2', '-D__WIIU__', '-D__WUT__', '-DESPRESSO', f'-I{PORT}/wiiu', f'-I{PORT}', f'-I{RT}', f'-I{RT}/audio', f'-I{RT}/gpu', f'-I{RT}/services',
            f'-I{CAFEGLSL_PPC}/include',
            '-isystem', f'{WUT}/include', '-include', f'{PORT}/wiiu/shim.h', '-DRECOMP_GAME="tomodachi"'] + extra


def obj_name(src):
    return '_project_' + os.path.relpath(src, ROOT).replace('\\', '/').replace('/', '_') + '.o'


def build_runtime(g, args):
    obj_dir = f'{WORK}/rt_{"tomo" if g.family == "tomo" else "std"}'
    os.makedirs(obj_dir, exist_ok=True)
    skip = {'frontend.cpp', 'main.cpp', 'host_sdl.cpp', 'watch.cpp', 'profile.cpp'}  # shadergen.cpp runs on the console too, for shader_compile.cpp
    sources = []
    for pat in (RT + '/*.cpp', RT + '/services/*.cpp', RT + '/gpu/*.cpp', RT + '/audio/*.cpp', PORT + '/wiiu/*.cpp'):
        sources += [s.replace('\\', '/') for s in sorted(glob.glob(pat)) if os.path.basename(s) not in skip]
    items = []
    extra = shlex.split(args.extra_flags or '')
    only = set((args.only or '').replace(' ', '').split(',')) - {''}
    for s in sources:
        base = os.path.basename(s)
        name = base
        flags = runtime_flags(extra) + (['-fpermissive', '-Daccelerometer=accelorometer'] if not WINDOWS else [])
        real = s
        if g.family == 'tomo':
            # no microphone: see build/tomo_shared.sh
            if base == 'mic_wiiu.cpp':
                continue
            if base == 'frontend_wiiu.cpp':
                real = f'{obj_dir}/frontend_wiiu.cpp'
                text = ''.join(line for line in open(s, encoding='utf-8', newline='') if 'wiiu::mic_blow' not in line)
                if not os.path.exists(real) or open(real, encoding='utf-8', newline='').read() != text:
                    open(real, 'w', encoding='utf-8', newline='').write(text)
        obj = f'{obj_dir}/{obj_name(s)}'
        if only and base not in only and os.path.exists(obj):
            continue
        items.append((name, real, obj, [tool('g++')] + flags))
    compile_all(items, args.jobs, 'runtime', args.force)
    return obj_dir


def game_flags(g, opt=None):
    return [tool('gcc')] + (shlex.split(opt) if opt else g.opt or ['-O2']) + ['-mcpu=750', '-meabi', '-mhard-float', '-ffp-contract=off', '-fno-math-errno',
                                                                                   '-fno-strict-aliasing', '-std=gnu11', '-w', f'-I{PORT}', f'-I{RT}', f'-I{g.gen_dir}']


def build_game_code(g, args):
    obj_dir = f'{WORK}/ppc_{g.name}'
    os.makedirs(obj_dir, exist_ok=True)
    flags = game_flags(g, args.opt)
    items = []
    for c in sorted(glob.glob(g.gen_dir + '/*.c')):
        c = c.replace('\\', '/')
        n = os.path.basename(c)[:-2]
        items.append((n, c, f'{obj_dir}/{n}.o', flags))
    # a new generation can come in fewer files: the old ones' objects would be linked too, defining their functions twice
    wanted = {n for n, *_ in items}
    for o in glob.glob(obj_dir + '/*.o*'):
        if os.path.basename(o).split('.')[0] not in wanted:
            os.remove(o)
    compile_all(items, args.jobs, 'game code', False)
    return obj_dir


def build_code(g, args):
    check_tools(code=True, game=g)
    if need_gen(g):
        print('recompiling the game to C')
        do_gen(g, args)
    link_setup()
    rt = build_runtime(g, args)
    code = build_game_code(g, args)
    link_game(g, rt, code)


def link_game(g, rt, code):
    os.makedirs(WORK + '/link', exist_ok=True)
    os.makedirs(g.out, exist_ok=True)
    def rel(p):  # the standalone builder can have its work folder on another drive than its tools: then there's no relative path
        try:
            return os.path.relpath(p, WORK).replace('\\', '/')
        except ValueError:
            return p.replace('\\', '/')
    aac = []
    if g.aac:
        a = ROOT + '/toolchain/aac_ppc'
        o = f'{WORK}/link/aac_ffmpeg.o'
        r = run([tool('g++'), '-mcpu=750', '-meabi', '-mhard-float', '-O2', '-std=c++20', '-D__WIIU__', f'-I{a}/include', '-c', PORT + '/aac/aac_ffmpeg.cpp', '-o', o])
        if r.returncode:
            sys.exit('aac_ffmpeg did not compile')
        aac = [rel(o), rel(a + '/lib/libavcodec.a'), rel(a + '/lib/libavutil.a')]
    objects = sorted(glob.glob(rt + '/*.o'))
    if g.family == 'tomo':
        objects = [o for o in objects if not o.replace('\\', '/').endswith(obj_name(PORT + '/wiiu/mic_wiiu.cpp'))]
    rsp = f'{WORK}/link/{g.name}.rsp'
    # the runtime goes first, so that its calls into the C library stay near
    open(rsp, 'w', newline='\n').write('\n'.join(rel(o) for o in objects))
    code_rsp = f'{WORK}/link/{g.name}_code.rsp'
    open(code_rsp, 'w', newline='\n').write('\n'.join(rel(o) for o in sorted(glob.glob(code + '/*.o'))))
    elf = f'link/{g.name}.elf'
    if os.path.exists(f'{WORK}/{elf}'):
        os.remove(f'{WORK}/{elf}')
    t0 = time.time()
    # a stamp the game logs at its start ([loader] build ...), so a console run says which build it is: an update title's or the base's
    stamp_c = f'{WORK}/link/stamp.c'
    open(stamp_c, 'w', newline='\n').write(f'const char g_build_stamp[] = "{time.strftime("%Y-%m-%d %H:%M")} {g.name}";\n')
    if run([tool('gcc'), '-mcpu=750', '-meabi', '-mhard-float', '-c', stamp_c, '-o', f'{WORK}/link/stamp.o']).returncode:
        sys.exit('the build stamp did not compile')
    extra = [rel(f'{WORK}/link/stamp.o')]
    # relative paths: a space in the project's path would break the specs file's -T
    # every file call goes to port/wiiu/fsa_direct.cpp first, the C library's too
    wraps = ['-Wl,--wrap=' + f for f in ('FSAOpenFileEx', 'FSACloseFile', 'FSAReadFile', 'FSAWriteFile', 'FSASetPosFile', 'FSAGetStatFile', 'FSAGetStat', 'FSAFlushFile', 'FSATruncateFile',
                                         'FSARemove', 'FSAMakeDir', 'FSARename', 'FSAOpenDir', 'FSAReadDir', 'FSARewindDir', 'FSACloseDir')]
    argv = [tool('g++'), '-mcpu=750', '-meabi', '-mhard-float', '-specs=rpx/rpx.specs', '-Wl,--relax', '-Wl,--no-warn-rwx-segments', f'@link/{g.name}.rsp'] + wraps + aac + extra
    argv += ['-Wl,--start-group', f'-L{WUT}/lib', f'-L{CAFEGLSL_PPC}/lib', '-lcafeglsl', '-lwut', '-lstdc++', '-lm', '-lc', '-lsysbase', '-lgcc', '-Wl,--end-group', f'@link/{g.name}_code.rsp', '-o', elf]
    r = run(argv, cwd=WORK, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    errors = sorted({re.sub(r'^.*ld(\.exe)?: ', '', x) for x in r.stdout.splitlines() if re.search(r'undefined reference|error|multiple definition|truncated', x)})
    for e in errors[:30]:
        print(e[:170])
    if not os.path.exists(f'{WORK}/{elf}'):
        sys.exit('link failed:\n' + r.stdout[-1500:])
    print(f'linked in {time.time() - t0:.0f} s')
    out = g.rpx
    with open(g.nm, 'wb') as f:
        run([tool('nm'), '-n', elf], cwd=WORK, stdout=f)
    r = run(['elf2rpl' + TOOL_EXT, elf, os.path.relpath(out, WORK).replace('\\', '/')], cwd=WORK, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    if r.returncode:
        sys.exit('elf2rpl failed:\n' + r.stdout[-1000:])
    size = run([tool('size'), elf], cwd=WORK, capture_output=True, text=True).stdout.splitlines()[-1][:70]
    heads = run([tool('objdump'), '-h', elf], cwd=WORK, capture_output=True, text=True).stdout
    print(size)
    # the last build known to boot on the console imported coreinit gx2 proc_ui sndcore2 sysapp vpad: anything else here is a suspect if a build hangs at its splash
    print('imports:', ' '.join(sorted(set(re.findall(r'\.fimport_([A-Za-z0-9_]+)', heads)))))
    os.remove(f'{WORK}/{elf}')
    print(f'{out}: {os.path.getsize(out)} bytes')


# ---------------------------------------------------------------- shaders
def build_shadergen(args):
    """The generator, for this PC, from the same sources as the other OS's build. It writes the GLSL and the index; compile_shaders does the rest."""
    if os.environ.get('THREEDII_SHADERGEN'):  # the standalone builder ships it built, as it has no host compiler
        os.makedirs(WORK + '/shadergen', exist_ok=True)  # build_shaders keeps a file there
        return os.environ['THREEDII_SHADERGEN']
    d = WORK + '/shadergen'
    os.makedirs(d, exist_ok=True)
    flags = HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{PORT}/wiiu'] + ([f'-I{PORT}/pc/win'] if WINDOWS else []) + [f'-I{PORT}/pc', f'-I{RT}', f'-I{RT}/gpu', '-include', f'{PORT}/pc/shim.h']
    sources = [ROOT + '/tools/shadergen_main.cpp', PORT + '/wiiu/shadergen.cpp', PORT + '/wiiu/shaderkeys.cpp', RT + '/gpu/shader.cpp', ROOT + '/tools/win/shadergen_stubs.cpp']
    items = [(os.path.basename(s), s, f'{d}/{os.path.basename(s)}.o', flags) for s in sources]
    compile_all(items, args.jobs, 'shader generator')
    exe = d + '/shadergen' + TOOL_EXT
    objs = [i[2] for i in items]
    if not os.path.exists(exe) or any(os.path.getmtime(o) > os.path.getmtime(exe) for o in objs):
        r = run(HOST_CXX + (['-static'] if WINDOWS else []) + [*objs, '-lpthread', '-lm', '-o', exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        if r.returncode:
            sys.exit('shadergen did not link:\n' + r.stdout[-1500:])
    return exe


GFD_HEADER, BLOCK_HEADER = 32, 32
VS_HEAD, VS_PROG, PS_HEAD, PS_PROG, GFD_END = 3, 5, 6, 7, 1


def gfd_blocks(data):
    if data[:4] != b'Gfx2':
        raise ValueError('not a GFD file')
    p = struct.unpack('>I', data[4:8])[0]
    while p + BLOCK_HEADER <= len(data) and data[p:p + 4] == b'BLK{':
        head, _, _, kind, size = struct.unpack('>5I', data[p + 4:p + 24])
        yield kind, data[p:p + head], data[p + head:p + head + size]
        p += head + size


def merge_gfd(paths, out):
    """Many small .gsh files into one, shader order kept. Windows can't take thousands of file names on one command line. Each block has a running id and a per-shader index."""
    first = open(paths[0], 'rb').read()
    res = [first[:GFD_HEADER]]
    block_id = 0
    counts = {VS_HEAD: 0, PS_HEAD: 0}
    end = None
    index = {VS_HEAD: 0, VS_PROG: 0, PS_HEAD: 0, PS_PROG: 0}
    for path in paths:
        for kind, head, body in gfd_blocks(open(path, 'rb').read()):
            if kind == GFD_END:
                end = (head, body)
                continue
            if kind not in index:
                raise ValueError(f'unexpected block type {kind} in {path}')
            res.append(head[:24] + struct.pack('>II', block_id, index[kind]) + head[32:] + body)
            block_id += 1
            index[kind] += 1
            if kind in counts:
                counts[kind] += 1
    if end is None:
        raise ValueError('no end block')
    res.append(end[0][:24] + struct.pack('>II', block_id, 0) + end[0][32:] + end[1])
    open(out, 'wb').write(b''.join(res))
    return counts


def check_registers(path):
    """GX2SetShaderMode leaves 48 registers for vertex shaders and 200 for pixel shaders. A shader asking for more can't start on a real GPU, and Cemu doesn't mind."""
    vs = ps = 0
    for kind, _, body in gfd_blocks(open(path, 'rb').read()):
        if kind == VS_HEAD:
            vs = max(vs, struct.unpack('>I', body[:4])[0] & 255)
        if kind == PS_HEAD:
            ps = max(ps, struct.unpack('>I', body[:4])[0] & 255)
    print(f'registers: vertex shaders use up to {vs} of 48, pixel shaders up to {ps} of 200')
    if not vs or vs > 48 or ps > 200:
        sys.exit('a shader needs more registers than the GPU gives it')


def compile_shaders(out, jobs):
    """out/glsl/list.txt, from shadergen, names every shader in order. All GLSL here declares its uniforms in blocks, and the renderer keeps the GPU in block mode."""
    glsl = out + '/glsl'
    names = open(glsl + '/list.txt').read().split()
    vs = [n for n in names if n.endswith('.vert')]
    ps = [n for n in names if n.endswith('.frag')]
    tmp = out + '/batches'
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    order, batches = [], []
    for flag, files in (('-vs', vs), ('-ps', ps)):
        for i in range(0, len(files), 100):
            o = f'{tmp}/{len(batches):05}.gsh'
            argv = [GLSLC]
            for n in files[i:i + 100]:
                argv += [flag, n]
            argv += ['-o', o]
            order.append(o)
            batches.append((os.path.basename(o), argv, glsl, None))
    failed = run_jobs(batches, jobs, 'shaders')
    if failed:
        sys.exit('shader compiler failed on ' + ', '.join(failed))
    counts = merge_gfd(order, out + '/shaders.gsh')
    if counts != {VS_HEAD: len(vs), PS_HEAD: len(ps)}:
        sys.exit(f'expected {len(vs)} vertex and {len(ps)} pixel shaders, the compiler made {counts}')
    shutil.rmtree(tmp)
    check_registers(out + '/shaders.gsh')
    print(f'shaders.gsh: {os.path.getsize(out + "/shaders.gsh") >> 10} KB, {len(vs)} vertex and {len(ps)} pixel shaders')


def prepare_logs(g):
    """What other runs recorded: fragment shaders depend on render state, not on the game, so what another game logged may save this one a fallback in scenes no run reached."""
    logs = g.dir + '/shader_logs'
    os.makedirs(logs, exist_ok=True)
    run_dir = f'{WORK}/run_{g.name}'
    for f in ('vs_log.bin', 'fs_log.bin'):
        if os.path.exists(f'{run_dir}/{f}'):
            shutil.copyfile(f'{run_dir}/{f}', f'{logs}/{f}')
    r = run([sys.executable, ROOT + '/tools/dvlb_to_log.py', g.dir], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    print(r.stdout[-300:].strip())
    r = run([sys.executable, ROOT + '/tools/tev_to_log.py', g.dir], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    print(r.stdout[-300:].strip())
    if r.returncode:
        sys.exit('tev_to_log failed')
    if g.family == 'tomo':
        return
    for other in sorted(glob.glob(ROOT + '/games/*/')):
        other = other.rstrip('/\\').replace('\\', '/')
        name = os.path.basename(other)
        if other == g.dir or (g.family == 'layton' and not name.startswith('layton_')):
            continue
        if os.path.exists(f'{other}/shader_logs/fs_log.bin'):
            shutil.copyfile(f'{other}/shader_logs/fs_log.bin', f'{logs}/other_{name}.bin')
        if g.family == 'mario':
            for f in glob.glob(f'{other}/shader_logs/*_fs.bin'):
                if not os.path.basename(f).startswith('other_') and os.path.basename(f) != 'models_fs.bin':  # that game's own copies of these: taking them nested the names without end; its models are its own
                    shutil.copyfile(f, f'{logs}/other_{name}_{os.path.basename(f)}')
    if g.family == 'layton':
        known = [f for f in glob.glob(logs + '/*.bin') if os.path.basename(f) not in ('dvlb.bin', 'vs_log.bin', 'variants.bin')]
        r = run([sys.executable, ROOT + '/tools/layton_fs_variants.py', logs + '/variants.bin'] + known, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        print(r.stdout[-300:].strip())


def build_shaders(g, args):
    check_tools(shaders=True)
    exe = build_shadergen(args)
    prepare_logs(g)
    os.makedirs(g.shaders, exist_ok=True)
    pipelines = g.pipelines
    if not pipelines and g.family == 'mario':
        import json
        tid = json.load(open(g.dir + '/info.json'))['program_id'].lower()
        p = f'{ROOT}/third_party/3dsrecomp/games/{tid}/pipelines.bin'
        pipelines = p if os.path.exists(p) else None
    empty = WORK + '/shadergen/no_pipelines.bin'
    open(empty, 'wb').close()
    r = run([exe, pipelines or empty, g.dir + '/shader_logs', g.shaders, '-'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    print(r.stdout[-600:].strip())
    if r.returncode:
        sys.exit('shadergen failed')
    compile_shaders(g.shaders, args.jobs)
    if g.family != 'tomo':
        shutil.rmtree(g.shaders + '/glsl', ignore_errors=True)


# ---------------------------------------------------------------- the PC trial: the game on this PC, headless, with the software renderer. It finds code static analysis missed and logs the shaders used.
def host_runtime(args):
    if not WINDOWS:
        sys.exit('Windows host trials are not implemented on Linux; Wii U code/shader/title/pack builds are supported')
    obj_dir = WORK + '/pc_rt'
    os.makedirs(obj_dir, exist_ok=True)
    flags = HOST_CXX + ['-std=c++20', '-O1', '-g', '-w', '-DRECOMP_VS_LOG', '-D_FILE_OFFSET_BITS=64', f'-I{PORT}/pc/win', f'-I{PORT}/wiiu', f'-I{PORT}/pc', f'-I{RT}', f'-I{RT}/audio', f'-I{RT}/gpu',
                        f'-I{RT}/services', '-include', f'{PORT}/pc/shim.h', '-DRECOMP_GAME="tomodachi"', f'-DRECOMP_SOURCE_DIR="{ROOT}/third_party/3dsrecomp"'] + shlex.split(args.extra_flags or '')
    skip = {'frontend.cpp', 'main.cpp', 'host_sdl.cpp', 'watch.cpp'}
    sources = []
    for pat in (RT + '/*.cpp', RT + '/services/*.cpp', RT + '/gpu/*.cpp', RT + '/audio/*.cpp', PORT + '/pc/*.cpp', PORT + '/pc/win/*.cpp'):
        sources += [s.replace('\\', '/') for s in sorted(glob.glob(pat)) if os.path.basename(s) not in skip]
    compile_all([(os.path.basename(s), s, f'{obj_dir}/{obj_name(s)}', flags) for s in sources], args.jobs, 'PC runtime', args.force)
    return obj_dir


def host_game_flags(gen_dir):
    return [LLVM + 'clang.exe', '--target=x86_64-w64-mingw32', '--sysroot=' + MINGW, '-O1', '-w', '-ffp-contract=off', '-fno-strict-aliasing', '-std=gnu11', f'-I{RT}', f'-I{gen_dir}']


def host_build(g, args):
    gen_dir = f'{g.dir}/{args.gen or "gen_safe"}'
    abi = gen_dir.endswith('gen_abi')
    table = f'{gen_dir}/table.c'
    tools = [f'{ROOT}/third_party/3dsrecomp/tools/recomp/{f}.py' for f in ('gen', 'cro')] + [g.extra_functions]
    if not os.path.exists(table) or (not g.keep_gen and any(os.path.exists(f) and os.path.getmtime(f) > os.path.getmtime(table) for f in tools)):
        print('recompiling the game to C')
        do_gen(g, args, gen_dir, abi)
    rt = host_runtime(args)
    obj_dir = f'{WORK}/pc_{g.name}_{os.path.basename(gen_dir)}'
    os.makedirs(obj_dir, exist_ok=True)
    flags = host_game_flags(gen_dir)
    items = []
    for c in sorted(glob.glob(gen_dir + '/*.c')):
        c = c.replace('\\', '/')
        n = os.path.basename(c)[:-2]
        items.append((n, c, f'{obj_dir}/{n}.o', flags))
    # clang 23's optimiser never finishes on the odd generated file (Miracle Mask's code_040: over 15 minutes at -O1, 2 s at -O0), and this build is only the reference
    compile_all(items, args.jobs, 'game code for the PC', timeout=240, fallback=['-O0'])
    exe = f'{obj_dir}/{g.name}.exe'
    objs = sorted(glob.glob(rt + '/*.o')) + sorted(glob.glob(obj_dir + '/*.o'))
    if not os.path.exists(exe) or any(os.path.getmtime(o) > os.path.getmtime(exe) for o in objs):
        t0 = time.time()
        rsp = f'{obj_dir}/link.rsp'
        open(rsp, 'w', newline='\n').write('\n'.join('"' + o.replace('\\', '/') + '"' for o in objs))
        # code compiled on the spot links back to the game and the runtime's helpers, through an import library. A .def, not --export-all-symbols: a program can only export 65535 names.
        names = run([LLVM + 'llvm-nm.exe', '-g', *glob.glob(obj_dir + '/table.o')], capture_output=True, text=True).stdout
        defined = set(re.findall(r'^[0-9a-f]+ [A-TV-Z] (\S+)$', names, re.M))
        helpers = re.findall(r'\b(rt_\w+|g_mem|guest_\w+)\b', open(RT + '/cpu.h').read())
        funcs = re.findall(r'^void (f_[0-9a-f]+)\(', open(gen_dir + '/guest.h').read(), re.M)
        if len(funcs) > 60000:  # over a program's 65535 exports (Animal Crossing's 98577): code compiled on the spot then can't call the game's
            funcs = []
        open(f'{obj_dir}/exports.def', 'w', newline='\n').write('EXPORTS\n' + '\n'.join(sorted(set(funcs) | set(helpers) | (defined & {'guest_entries', 'guest_entry_count'}))) + '\n')
        r = run(HOST_CXX + ['-static', f'@{rsp}', f'{obj_dir}/exports.def', '-lpthread', '-lwinmm', f'-Wl,--out-implib,{obj_dir}/game.dll.a', '-o', exe],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        if r.returncode:
            found = sorted({x for x in r.stdout.splitlines() if 'undefined symbol' in x or 'error' in x})
            sys.exit('the PC build did not link:\n' + '\n'.join(x[:170] for x in found[:40]))
        print(f'linked in {time.time() - t0:.0f} s: {os.path.getsize(exe) >> 20} MB')
    return exe, gen_dir, obj_dir


def do_lazy(g, address, run_dir, gen_dir, obj_dir):
    """Code the static analysis missed, found while running: into a DLL, with what it needs from the game coming from the running program."""
    os.makedirs(run_dir + '/lazy', exist_ok=True)
    base = f'{run_dir}/lazy/lazy_{address}'
    r = run([sys.executable, ROOT + '/tools/win/gen_lf.py', g.dir, gen_dir, '--lazy', address, '--lazy-out', base + '.c'], cwd=ROOT + '/third_party/3dsrecomp', env=game_env(g),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    if r.returncode:
        sys.exit(r.stdout[-800:])
    flags = host_game_flags(gen_dir) + ['-fuse-ld=lld', '-shared', base + '.c', f'{obj_dir}/game.dll.a', '-o', base + '.dll']
    r = run(flags, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    if r.returncode:
        sys.exit(r.stdout[-800:])


def trial(g, args):
    exe, gen_dir, obj_dir = host_build(g, args)
    if args.build_only:
        return
    run_dir = f'{WORK}/run_{g.name}' + (f'_{args.run}' if args.run else '')
    os.makedirs(run_dir, exist_ok=True)
    for d in ('save', 'shots'):
        shutil.rmtree(f'{run_dir}/{d}', ignore_errors=True)
    if args.save_from:
        shutil.copytree(args.save_from, f'{run_dir}/save')
    frames = int(args.frames or 3600)
    shots = args.shot_every or 300
    env = game_env(g)
    env.update(RECOMP_AUTOPLAY=str(args.autoplay), RECOMP_NOAUDIO='1', RECOMP_FRAMES=str(frames), RECOMP_SHOT_EVERY=str(shots), RECOMP_SHOT_DIR=run_dir + '/shots', GPU_THREADS=str(args.gpu_threads))
    if args.seed:
        env['RECOMP_SEED'] = str(args.seed)
    if args.speed:
        env['RECOMP_PCSPEED'] = str(args.speed)
    for item in args.env or []:
        k, v = item.split('=', 1)
        env[k] = v
    with open(run_dir + '/lazy.cmd', 'w', newline='\r\n') as f:
        f.write(f'@"{sys.executable}" "{ROOT}/tools/win/build3ds.py" lazy {g.name} %1 --gen "{os.path.basename(gen_dir)}"' + (f' --run {args.run}' if args.run else '') + '\n')
    print(f'running {frames} frames, autoplay {args.autoplay}', flush=True)
    code = 'timed out'
    with open(run_dir + '/run.log', 'wb') as log:
        p = subprocess.Popen([exe, g.dir], cwd=run_dir, env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=BELOW_NORMAL)
        try:
            code = p.wait(timeout=args.seconds or frames * 100 // (args.speed or 100) // 60 + 40)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    text = open(run_dir + '/run.log', errors='replace').read().splitlines()
    print('exit', code)
    for line in [x for x in text if not re.search(r'\[thread |frame |ControlMemory|recompiling missing', x)][-(args.log_lines or 16):]:
        print(line[:170])
    frames_seen = [x for x in text if ' frame ' in x]
    if frames_seen:
        print(frames_seen[-1][:110])
    missing = f'{run_dir}/save/missing_functions.txt'
    if os.path.exists(missing) and 'RECOMP_TEST_LAZY' not in env:
        have = set(open(g.extra_functions).read().split()) if os.path.exists(g.extra_functions) else set()
        new = {x for x in open(missing).read().split() if not re.match(r'0x00000[0-9a-f]*$', x)}
        if not new <= have:
            open(g.extra_functions, 'w', newline='\n').write('\n'.join(sorted(have | new)) + '\n')
        print(f'functions found at run time so far: {len(have | new)}')
    if glob.glob(run_dir + '/shots/top*.ppm'):
        os.makedirs(g.dir + '/shots', exist_ok=True)
        sheet = f'{g.dir}/shots/sheet{"_" + args.run if args.run else ""}.png'
        run([sys.executable, ROOT + '/tools/contact_sheet.py', 'shots', sheet, '8'], cwd=run_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print('pictures:', sheet)
    return code


# ---------------------------------------------------------------- dependencies
def missing_tools(code=False, shaders=False, game=None):
    missing = []
    if code:
        if not os.path.isfile(tool('gcc')) or not os.path.isfile(tool('g++')):
            missing.append('devkitPPC gcc/g++ (set THREEDII_DEVKITPRO or DEVKITPRO)')
        if not os.path.isfile(WUT + '/share/wut.ld') or not os.path.isfile(WUT + '/lib/libwut.a'):
            missing.append('wut Wii U SDK (under the devkitPro directory)')
        if not os.path.isfile(CAFEGLSL_PPC + '/lib/libcafeglsl.a'):
            missing.append('CafeGLSL Wii U library (set THREEDII_CAFEGLSL_PPC)')
        if not shutil.which('elf2rpl' + TOOL_EXT):
            missing.append('devkitPro elf2rpl / wut-tools')
        if game and game.aac and not (os.path.isfile(ROOT + '/toolchain/aac_ppc/lib/libavcodec.a') and
                                      os.path.isfile(ROOT + '/toolchain/aac_ppc/lib/libavutil.a')):
            missing.append('Wii U AAC decoder libraries (toolchain/aac_ppc)')
    if shaders:
        if not os.path.isfile(GLSLC):
            missing.append('CafeGLSL v0.2.0 native shader compiler (set THREEDII_GLSLC)')
        if not os.environ.get('THREEDII_SHADERGEN') and not shutil.which('clang++') and not shutil.which('g++'):
            missing.append('native C++ compiler for shadergen (clang++ or g++)')
        if os.environ.get('THREEDII_SHADERGEN') and not os.path.isfile(os.environ['THREEDII_SHADERGEN']):
            missing.append('shader generator specified by THREEDII_SHADERGEN')
    return missing


def check_tools(code=False, shaders=False, game=None):
    missing = missing_tools(code, shaders, game)
    if missing:
        sys.exit('Missing tools:\n  - ' + '\n  - '.join(missing) + '\nSee builder/README.md.')


# ---------------------------------------------------------------- title and WUP
def build_title(g, args):
    if g.family == 'tomo':
        argv = [sys.executable, ROOT + '/tools/make_game_title.py']
    elif g.family == 'layton':
        argv = [sys.executable, ROOT + '/tools/layton_title.py', g.name]
    else:
        argv = [sys.executable, ROOT + '/tools/mario_title.py', g.name]
    if run(argv, cwd=ROOT).returncode:
        sys.exit('the title script failed')


def pack(g, otp):
    title = g.out + '/title'
    if g.family == 'tomo':
        target = ROOT + '/dist/Tomodachi_Life'
    else:
        target = f'{ROOT}/dist/{g.name}'
    r = subprocess.run([sys.executable, ROOT + '/tools/pack_game_wup.py', otp, title, target], cwd=ROOT)
    if r.returncode:
        sys.exit('packing failed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['doctor', 'code', 'shaders', 'title', 'all', 'pack', 'gen', 'trial', 'lazy'])
    ap.add_argument('game', nargs='?')
    ap.add_argument('otp', nargs='?', help="pack: your otp.bin. trial: frames, default 3600. lazy (the trial's own use): an address")
    ap.add_argument('--gen', help='trial: which generated code, gen_safe (default) or gen_abi')
    ap.add_argument('--autoplay', type=int, default=2, help='trial: 1 presses A and taps, 2 also wanders, 3 walks about')
    ap.add_argument('--seed', type=int)
    ap.add_argument('--speed', type=int, help='trial: percent. 200 gets further in the same time')
    ap.add_argument('--gpu-threads', type=int, default=4, help='trial: threads for the software renderer')
    ap.add_argument('--run', help='trial: a name of its own, so copies can run side by side')
    ap.add_argument('--save-from', help='trial: start from a copy of this save folder')
    ap.add_argument('--env', action='append', help='trial: more settings, as NAME=value')
    ap.add_argument('--shot-every', type=int)
    ap.add_argument('--seconds', type=int, help='trial: stop after this long anyway')
    ap.add_argument('--log-lines', type=int)
    ap.add_argument('--build-only', action='store_true', help='trial: build it and stop')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--extra-flags')
    ap.add_argument('--only')
    ap.add_argument('--opt')
    ap.add_argument('--out', help='put the RPX, game.nm and shaders here instead, to keep a test build apart')
    args = ap.parse_args()
    if args.command == 'doctor':
        g = Game(args.game) if args.game else None
        missing = missing_tools(code=True, shaders=True, game=g)
        print('Platform:', sys.platform)
        print('devkitPro:', DKP)
        print('CafeGLSL host:', GLSLC)
        print('CafeGLSL Wii U:', CAFEGLSL_PPC)
        if missing:
            for item in missing:
                print('MISSING:', item)
            sys.exit(1)
        print('Core build tools found. Dependencies are ready for a real game build.')
        return
    if not args.game or not os.path.isdir(f'{ROOT}/games/{args.game}'):
        sys.exit(f'no games/{args.game}')
    if not WINDOWS and args.command in ('trial', 'lazy'):
        sys.exit('Linux host trials/lazy recompilation are not supported yet')
    g = Game(args.game)
    if args.out:
        g.out = args.out.replace('\\', '/')
        g.rpx, g.nm, g.shaders = f'{g.out}/{g.name}.rpx', f'{g.out}/game.nm', f'{g.out}/shaders'
    if args.command == 'gen':
        do_gen(g, args)
    if args.command in ('code', 'all'):
        build_code(g, args)
    if args.command in ('shaders', 'all'):
        build_shaders(g, args)
    if args.command in ('title', 'all'):
        build_title(g, args)
    if args.command == 'pack':
        pack(g, args.otp or sys.exit('pack needs the path to your otp.bin'))
    if args.command == 'trial':
        args.frames = args.otp
        trial(g, args)
    if args.command == 'lazy':
        gen_dir = f'{g.dir}/{args.gen or "gen_safe"}'
        do_lazy(g, args.otp, f'{WORK}/run_{g.name}' + (f'_{args.run}' if args.run else ''), gen_dir, f'{WORK}/pc_{g.name}_{os.path.basename(gen_dir)}')


if __name__ == '__main__':
    main()
