#!/usr/bin/env python3
"""Builds the 3DS-to-Wii-U ports on Windows, with no WSL. Run with Windows Python from anywhere:

    build3ds.py code <game>            runtime, game code and link: the RPX and game.nm
    build3ds.py shaders <game>         the shader pack
    build3ds.py title <game>           the title folder, for Cemu and the WUP packer
    build3ds.py all <game>             those three
    build3ds.py pack <game> <otp.bin>  the WUP folder, in dist/
    build3ds.py gen <game>             the game's ARM code to C again
    build3ds.py trial <game> [frames]  the game on this PC, headless: finds code static analysis missed and logs the shaders used. See --help for its options.

Options: --jobs N (default 4), --force, --extra-flags "...", --only a.cpp,b.cpp (recompile only these runtime files), --opt -O1, --out <folder> (keep a test build apart).

Compiles run at below-normal priority, as many as --jobs, and none starts while the PC is short of free memory, so the PC stays usable.
Objects go in build_win/. Outputs stay where the title scripts and tools/console_watch.py expect them: build/ for Tomodachi, build_mario/<game>/,
build_layton/<game>/. Needs tools/win/install_toolchain.sh run once, C:\\msys64 with mingw-w64-x86_64-gcc and LLVM in C:\\Program Files\\LLVM for the trial and the shader generator.
Game and runtime objects came out byte-identical to the ones WSL's copy of the same compiler made.
"""
import argparse
import ctypes
import functools
import glob
import json
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # loaded by path (pipeline.py), so its own folder isn't on the path
import objcache  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace('\\', '/')
WORK = ROOT + '/build_win'
RT = ROOT + '/third_party/3dsrecomp/runtime'
PORT = ROOT + '/port'
DKP = os.environ.get('THREEDII_DEVKITPRO') or 'C:/devkitPro/opt/devkitpro'  # the standalone builder brings its own copy
BIN = DKP + '/devkitPPC/bin/powerpc-eabi-'
WUT = DKP + '/wut'
LLVM = 'C:/Program Files/LLVM/bin/'
MINGW = 'C:/msys64/mingw64'
GLSLC = ROOT + '/third_party/cafeglsl/glslcompiler.exe'  # v0.2.0, the compiler the console has run: tools/win/build_cafeglsl.ps1
CAFEGLSL_PPC = ROOT + '/third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc'  # the same compiler as a library for the console: port/wiiu/shader_compile.cpp. Once installed, run tools/win/patch_cafeglsl_traps.py: Cemu stops at its traps
HOST_CXX = [LLVM + 'clang++.exe', '--target=x86_64-w64-mingw32', '--sysroot=' + MINGW, '-fuse-ld=lld']
CRO_BASE = '0x1C000000'  # the slots CRO modules' code gets (tools/recomp/cro.py), in the part of the address space the Wii U port leaves free
FRAGMENT_VARIANTS = 12000  # how many lit-state combinations a game may get compiled ahead (prepare_logs): more and the build and the shader file grow too big
BELOW_NORMAL = 0x4000
MIN_FREE_MB = 2500

os.environ['DEVKITPRO'] = DKP
os.environ['DEVKITPPC'] = DKP + '/devkitPPC'
os.environ['PATH'] = DKP + '/devkitPPC/bin;' + DKP + '/tools/bin;' + os.environ['PATH']


class MemoryStatus(ctypes.Structure):
    _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong), ('avail', ctypes.c_ulonglong),
                ('total_page', ctypes.c_ulonglong), ('avail_page', ctypes.c_ulonglong), ('total_virtual', ctypes.c_ulonglong),
                ('avail_virtual', ctypes.c_ulonglong), ('avail_extended', ctypes.c_ulonglong)]


def free_mb():
    m = MemoryStatus()
    m.length = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.avail >> 20


def run(argv, **kw):
    return subprocess.run(argv, creationflags=BELOW_NORMAL, **kw)


context = threading.local()  # what a thread's output belongs to (pipeline.py makes shaders alongside the code); run_jobs' workers take their caller's


def run_jobs(jobs, n, what, timeout=None, fallback=None):
    """jobs: (label, step, cwd or None, called on success or None). A step is a command, or a function that runs the step itself and returns its
    CompletedProcess (cached_compile). Returns the labels that failed. Waits for free memory before starting each, unless nothing else is running.
    A command taking longer than timeout seconds is run again with the fallback flags added."""
    if not jobs:
        return []
    lock = threading.Lock()
    state = {'running': 0, 'done': 0}
    failed = []
    t0 = time.time()
    tag = getattr(context, 'tag', None)

    def one(job):
        context.tag = tag
        label, step, cwd, after = job
        while True:
            with lock:
                if state['running'] == 0 or free_mb() > MIN_FREE_MB:
                    state['running'] += 1
                    break
            time.sleep(2)
        if callable(step):
            r = step()
        else:
            try:
                r = run(step, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=timeout)
            except subprocess.TimeoutExpired:
                print(f'  {label}: over {timeout} s, trying {" ".join(fallback)}', flush=True)
                r = run(step + fallback, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
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


def obj_cache():
    """compiled objects by what went into them (objcache.py), beside the work folder so a new builder engine keeps them"""
    return os.environ.get('THREEDII_OBJCACHE') or os.path.join(os.path.dirname(WORK), 'objcache')


def cached_compile(src, obj, argv, timeout, fallback, hits, force=False):
    """compiles src unless an object made from the same preprocessed code with the same flags is in the cache (not when forced)"""
    began = time.time()
    k = objcache.key(argv, src, obj + '.d')
    if k and not force and objcache.fetch(obj_cache(), k, obj):
        hits.append(src)
        return subprocess.CompletedProcess(argv, 0, '')
    cmd = argv + ['-c', src, '-o', obj, '-MMD', '-MF', obj + '.d']
    slow = False
    try:
        r = run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f'  {os.path.basename(src)}: over {timeout} s, trying {" ".join(fallback)}', flush=True)
        r = run(cmd + fallback, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        slow = True  # made with other flags than the key says
    # kept only if nothing it read changed while it compiled: other sessions edit runtime headers during builds, and an object of the new code
    # under the old code's key would come back whenever the old code did (ccache's rule)
    if not r.returncode and k and not slow and all(os.path.exists(d) and os.path.getmtime(d) < began for d in (read_deps(obj + '.d') or []) + [src]):
        objcache.store(obj_cache(), k, obj)
    return r


def compile_all(items, n, what, force=False, timeout=None, fallback=None):
    """items: (label, source, object, argv without the output or dependency file)"""
    jobs, hits = [], []
    for label, src, obj, argv in items:
        if stale(obj, src, argv, force):
            def remember(r, obj=obj, argv=argv):
                open(obj + '.cmd', 'w').write(' '.join(argv))
            jobs.append((label, lambda src=src, obj=obj, argv=argv: cached_compile(src, obj, argv, timeout, fallback or [], hits, force), None, remember))
    print(f'{what}: {len(items) - len(jobs)} of {len(items)} up to date', flush=True)
    if run_jobs(jobs, n, what, timeout, fallback):
        sys.exit(f'{what}: compile errors above')
    if hits:  # worded so pipeline.py's progress reader ("<what>: N of M") doesn't take it for progress
        print(f'{what}: the cache of compiled code had {len(hits)} of them', flush=True)
    if jobs:
        objcache.trim(obj_cache())


def mobiclip_tables(game_dir):
    """Offset-table jumps (`sub rB, pc, #n` ... `add pc, rX, rB`) in the game's code. Mobiclip's hand-written video decoder is made of them
    (168 in Mario Kart 7, the Laytons and Paper Jam): it keeps values in any register across its own calls, which only code generated
    without --abi follows. Mario Kart 7's menu videos stopped the game otherwise."""
    with open(os.path.join(game_dir, 'exheader.bin'), 'rb') as f:
        size = struct.unpack_from('<I', f.read(0x40), 0x18)[0]
    with open(os.path.join(game_dir, 'code.bin'), 'rb') as f:
        code = f.read(size)
    words = struct.unpack_from('<%dI' % (len(code) // 4), code)
    n = 0
    for i, w in enumerate(words):
        if w >> 28 != 0xE or (w & 0x0FEF0000) != 0x024F0000:  # sub/add rB, pc, #n
            continue
        rb = (w >> 12) & 15
        for x in words[i + 1:i + 16]:
            if (x & 0xFFF0FFF0) == 0xE080F000 and rb in ((x >> 16) & 15, x & 15):  # add pc, rX, rB
                n += 1
                break
            if (x >> 26) & 3 == 0 and (x >> 12) & 15 == rb:  # rB written again first
                break
    return n


def coroutine_switches(game_dir):
    """Calls to setjmp followed within a few instructions by a call to longjmp: a switch to another stack, as script tasks built on them do
    (Ace Attorney Trilogy, 2026-11-09). The longjmp lands in a frame the recompiled code has already left, and only code with a resume point
    at every return site (not --abi) can go on from there. An error handler's setjmp has no longjmp next to it (Metroid's), and stays --abi."""
    with open(os.path.join(game_dir, 'exheader.bin'), 'rb') as f:
        size = struct.unpack_from('<I', f.read(0x40), 0x18)[0]
    with open(os.path.join(game_dir, 'code.bin'), 'rb') as f:
        code = f.read(size)
    words = struct.unpack_from('<%dI' % (len(code) // 4), code)
    base = 0x100000  # where the code is loaded

    def call_target(i):
        """the function a bl at word i calls, or None"""
        w = words[i]
        if (w & 0x0F000000) != 0x0B000000 or w >> 28 == 0xF:
            return None
        off = (w & 0xFFFFFF) << 2
        to = base + i * 4 + 8 + (off - 0x4000000 if off & 0x2000000 else off)
        return to if base <= to < base + len(code) else None

    def stores_lr(w):  # stmia r0 or r0!, {..., lr}
        return w >> 28 == 0xE and (w & 0x0FFF0000) in (0x08A00000, 0x08800000) and w & 1 << 14

    @functools.cache
    def is_setjmp(a):
        """r0's buffer gets lr, and sp, in the function's first three instructions"""
        head = words[(a - base) // 4:(a - base) // 4 + 3]
        return len(head) == 3 and stores_lr(head[0]) and any(w in (0xE480D004, 0xE580D000)  # str sp, [r0], #4 or str sp, [r0]
                                                            or (stores_lr(w) and w & 1 << 13) for w in head)  # or sp in the stm

    @functools.cache
    def is_longjmp(a):
        """sp and lr come back from memory, then bx lr, in the function's first 20 instructions"""
        body = words[(a - base) // 4:(a - base) // 4 + 20]
        return (any((w & 0xFFFFFFF0) == 0xE1A0D000 for w in body)  # mov sp, rN
                and 0xE12FFF1E in body  # bx lr
                and any((w >> 25) & 7 == 4 and w & 1 << 20 and w & 1 << 14 for w in body))  # ldm {..., lr}

    switches = 0
    for i in range(len(words)):
        t = call_target(i)
        if t is not None and is_setjmp(t) and any((u := call_target(j)) is not None and is_longjmp(u) for j in range(i + 1, min(i + 17, len(words)))):
            switches += 1
    return switches


class Game:
    """How a game builds. Every game follows the same recipe: what differs comes from the game itself (its code, its program id) or from what
    earlier runs of it learned (functions.txt, shader logs), never from which game it is."""
    def __init__(self, name, game_dir=None):
        self.name = name
        self.dir = (game_dir or f'{ROOT}/games/{name}').replace('\\', '/')
        # where the build goes: the folders the first ports were built in, which tools/check_ports.py and tools/ship_card.py look in
        self.out = f'{ROOT}/build' if name == 'tomodachi' else f'{ROOT}/build_layton/{name}' if name.startswith('layton_') else f'{ROOT}/build_mario/{name}'
        self.rpx, self.nm, self.shaders = f'{self.out}/{name}.rpx', f'{self.out}/game.nm', f'{self.out}/shaders'
        self.extra_functions = f'{self.dir}/functions.txt'  # what runs of the game found that static analysis missed
        # what the 3dsrecomp project's own runs of this game learned: its functions and the render states it uses
        with open(f'{self.dir}/info.json', encoding='utf-8') as f:
            upstream = f'{ROOT}/third_party/3dsrecomp/games/{json.load(f)["program_id"].lower()}'
        self.upstream_functions = f'{upstream}/functions.txt' if os.path.isfile(f'{upstream}/functions.txt') else None
        self.pipelines = f'{upstream}/pipelines.bin' if os.path.isfile(f'{upstream}/pipelines.bin') else None
        # Hand-written code that keeps values in any register across its own calls (Mobiclip's video decoder) runs right only when every register
        # goes through memory at calls: safe mode. gcc -O2 turns that C into code Cemu's recompiler runs wrong (a compare reused across a jump), so -O1.
        safe = mobiclip_tables(self.dir) >= 8 or coroutine_switches(self.dir) > 0
        self.gen, self.mode, self.opt = ('gen_safe', 'safe', ['-O1']) if safe else ('gen_abi', 'abi', [])
        self.gen_dir = f'{self.dir}/{self.gen}'


# ---------------------------------------------------------------- recompiling the game's ARM code to C
def recompiler_sources():
    return glob.glob(f'{ROOT}/third_party/3dsrecomp/tools/recomp/*.py')


def functions_list(g):
    """The recompiler's --extra: the entries this game's runs found, and the 3dsrecomp project's runs of it if it has any.
    A merged list is written only when it changes, so its date says when the entries did."""
    lists = [p for p in (g.extra_functions, g.upstream_functions) if p and os.path.isfile(p) and os.path.getsize(p)]
    if len(lists) == 1:
        return lists[0]
    merged = f'{WORK}/functions_{g.name}.txt'
    text = ''.join(open(p, encoding='utf-8').read().strip() + '\n' for p in lists)
    if not os.path.isfile(merged) or open(merged, encoding='utf-8').read() != text:
        os.makedirs(WORK, exist_ok=True)
        with open(merged, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    return merged


def need_gen(g):
    """the C is made again whenever the recompiler or the game's entries are newer than it, for every game"""
    table = f'{g.gen_dir}/table.c'
    if not os.path.exists(table):
        return True
    t = os.path.getmtime(table)
    return any(os.path.getmtime(f) > t for f in recompiler_sources() + [functions_list(g)])


def game_env(g):
    return dict(os.environ, RECOMP_CRO_BASE=CRO_BASE)


def do_gen(g, args, gen_dir=None, abi=None):
    argv = [sys.executable, ROOT + '/tools/win/gen_lf.py', g.dir, gen_dir or g.gen_dir, '--extra', functions_list(g)]
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
            '-isystem', f'{WUT}/include', '-include', f'{PORT}/wiiu/shim.h'] + extra


def obj_name(src):
    return '_project_' + os.path.relpath(src, ROOT).replace('\\', '/').replace('/', '_') + '.o'


def build_runtime(g, args):
    """the runtime every game links: one build of it serves them all"""
    obj_dir = f'{WORK}/rt_std'
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
        flags = runtime_flags(extra)
        real = s  # (mic_wiiu.cpp loads mic.rpl when a game first asks for the mic, so no game imports it: importing it hung a big build before main)
        obj = f'{obj_dir}/{obj_name(s)}'
        if only and base not in only and os.path.exists(obj):
            continue
        items.append((name, real, obj, [BIN + 'g++.exe'] + flags))
    compile_all(items, args.jobs, 'runtime', args.force)
    return obj_dir


def game_flags(g, opt=None):
    return [BIN + 'gcc.exe'] + (shlex.split(opt) if opt else g.opt or ['-O2']) + ['-mcpu=750', '-meabi', '-mhard-float', '-ffp-contract=off', '-fno-math-errno',
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
    # the decoder behind the DSP's AAC pipe, in every game: a game that streams AAC is silent without it, and one that doesn't only pays its size
    a = ROOT + '/toolchain/aac_ppc'
    o = f'{WORK}/link/aac_ffmpeg.o'
    r = run([BIN + 'g++.exe', '-mcpu=750', '-meabi', '-mhard-float', '-O2', '-std=c++20', '-D__WIIU__', f'-I{a}/include', '-c', PORT + '/aac/aac_ffmpeg.cpp', '-o', o])
    if r.returncode:
        sys.exit('aac_ffmpeg did not compile')
    aac = [rel(o), rel(a + '/lib/libavcodec.a'), rel(a + '/lib/libavutil.a')]
    objects = sorted(glob.glob(rt + '/*.o'))
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
    if run([BIN + 'gcc.exe', '-mcpu=750', '-meabi', '-mhard-float', '-c', stamp_c, '-o', f'{WORK}/link/stamp.o']).returncode:
        sys.exit('the build stamp did not compile')
    extra = [rel(f'{WORK}/link/stamp.o')]
    # relative paths: a space in the project's path would break the specs file's -T
    # every file call goes to port/wiiu/fsa_direct.cpp first, the C library's too
    wraps = ['-Wl,--wrap=' + f for f in ('FSAOpenFileEx', 'FSACloseFile', 'FSAReadFile', 'FSAWriteFile', 'FSASetPosFile', 'FSAGetStatFile', 'FSAGetStat', 'FSAFlushFile', 'FSATruncateFile',
                                         'FSARemove', 'FSAMakeDir', 'FSARename', 'FSAOpenDir', 'FSAReadDir', 'FSARewindDir', 'FSACloseDir')]
    argv = [BIN + 'g++.exe', '-mcpu=750', '-meabi', '-mhard-float', '-specs=rpx/rpx.specs', '-Wl,--relax', '-Wl,--no-warn-rwx-segments', f'@link/{g.name}.rsp'] + wraps + aac + extra
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
        run([BIN + 'nm.exe', '-n', elf], cwd=WORK, stdout=f)
    r = run(['elf2rpl.exe', elf, os.path.relpath(out, WORK).replace('\\', '/')], cwd=WORK, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    if r.returncode:
        sys.exit('elf2rpl failed:\n' + r.stdout[-1000:])
    size = run([BIN + 'size.exe', elf], cwd=WORK, capture_output=True, text=True).stdout.splitlines()[-1][:70]
    heads = run([BIN + 'objdump.exe', '-h', elf], cwd=WORK, capture_output=True, text=True).stdout
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
    flags = HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{PORT}/wiiu', f'-I{PORT}/pc/win', f'-I{PORT}/pc', f'-I{RT}', f'-I{RT}/gpu', '-include', f'{PORT}/pc/shim.h']
    sources = [ROOT + '/tools/shadergen_main.cpp', PORT + '/wiiu/shadergen.cpp', PORT + '/wiiu/shaderkeys.cpp', RT + '/gpu/shader.cpp', ROOT + '/tools/win/shadergen_stubs.cpp']
    items = [(os.path.basename(s), s, f'{d}/{os.path.basename(s)}.o', flags) for s in sources]
    compile_all(items, args.jobs, 'shader generator')
    exe = d + '/shadergen.exe'
    objs = [i[2] for i in items]
    if not os.path.exists(exe) or any(os.path.getmtime(o) > os.path.getmtime(exe) for o in objs):
        r = run(HOST_CXX + ['-static', *objs, '-lpthread', '-o', exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
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
    import importlib.util
    spec = importlib.util.spec_from_file_location('fs_variants', ROOT + '/tools/fs_variants.py')
    fsv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fsv)
    own_logs = [f for f in glob.glob(logs + '/*.bin') if not os.path.basename(f).startswith('other_') and os.path.basename(f) not in ('dvlb.bin', 'variants.bin')]
    own = {k for f in own_logs for k in fsv.keys(f)}
    near = []  # the logs of games whose states this one shares: an engine's
    for other in sorted(glob.glob(ROOT + '/games/*/')):
        other = other.rstrip('/\\').replace('\\', '/')
        name = os.path.basename(other)
        if other == g.dir:
            continue
        # fs_log.bin and the *_fs.bin files hold fragment states alone; the other_ ones are that game's own copies (taking them nested the names without
        # end), and its models are its own
        theirs = [f for f in glob.glob(f'{other}/shader_logs/*.bin') if (os.path.basename(f) == 'fs_log.bin' or f.endswith('_fs.bin'))
                  and not os.path.basename(f).startswith('other_') and os.path.basename(f) != 'models_fs.bin']
        copied = []
        for f in theirs:
            to = f'{logs}/other_{name}.bin' if os.path.basename(f) == 'fs_log.bin' else f'{logs}/other_{name}_{os.path.basename(f)}'
            shutil.copyfile(f, to)
            copied.append(to)
        states = {k for f in theirs for k in fsv.keys(f)}
        if states and len(states & own) >= 0.05 * len(states):  # 7% and more within an engine, 3% and less across them (the nine, 2026-11)
            near += copied
    # combinations of the lit parts runs met, made from this game's states and its engine's: another engine's parts only multiply them
    r = run([sys.executable, ROOT + '/tools/fs_variants.py', logs + '/variants.bin', '--max', str(FRAGMENT_VARIANTS)] + own_logs + near,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    print(r.stdout[-300:].strip())


def build_shaders(g, args):
    if not os.path.exists(GLSLC):
        sys.exit('no shader compiler yet: run tools/win/build_cafeglsl.ps1 once')
    exe = build_shadergen(args)
    prepare_logs(g)
    os.makedirs(g.shaders, exist_ok=True)
    empty = WORK + '/shadergen/no_pipelines.bin'
    open(empty, 'wb').close()
    r = run([exe, g.pipelines or empty, g.dir + '/shader_logs', g.shaders, '-'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    print(r.stdout[-600:].strip())
    if r.returncode:
        sys.exit('shadergen failed')
    compile_shaders(g.shaders, args.jobs)
    shutil.rmtree(g.shaders + '/glsl', ignore_errors=True)  # compiled into shaders.gsh; nothing reads the GLSL after


# ---------------------------------------------------------------- the PC trial: the game on this PC, headless, with the software renderer. It finds code static analysis missed and logs the shaders used.
def host_runtime(args):
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
    if not os.path.exists(table) or any(os.path.getmtime(f) > os.path.getmtime(table) for f in recompiler_sources() + [functions_list(g)]):
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
        r = run(HOST_CXX + ['-static', f'@{rsp}', f'{obj_dir}/exports.def', '-lpthread', '-lwinmm', '-lgdi32', f'-Wl,--out-implib,{obj_dir}/game.dll.a', '-o', exe],
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
    for d in ('shots',) if args.keep_save else ('save', 'shots'):
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


# ---------------------------------------------------------------- title and WUP
def known_game(name):
    """a tested game's identity on the Wii U (title id, Menu name, product code, WUP folder), from builder/compat.json, the one list of them"""
    import json
    with open(ROOT + '/builder/compat.json', encoding='utf-8') as f:
        for entry in json.load(f).values():
            if isinstance(entry, dict) and entry.get('slug') == name:
                return entry
    sys.exit(f"{name} isn't in builder/compat.json")


def build_title(g, args):
    import importlib.util
    spec = importlib.util.spec_from_file_location('make_game_title', ROOT + '/tools/make_game_title.py')
    titles = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(titles)
    info = known_game(g.name)
    titles.make_title(g.out + '/title', g.dir, g.rpx, g.shaders, info['title_id'], info['name'], info['product'])


def pack(g, otp):
    title = g.out + '/title'
    target = f'{ROOT}/dist/{known_game(g.name)["folder"]}'  # as tools/ship_card.py names them
    r = subprocess.run([sys.executable, ROOT + '/tools/pack_game_wup.py', otp, title, target], cwd=ROOT)
    if r.returncode:
        sys.exit('packing failed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['code', 'shaders', 'title', 'all', 'pack', 'gen', 'trial', 'lazy'])
    ap.add_argument('game')
    ap.add_argument('otp', nargs='?', help="pack: your otp.bin. trial: frames, default 3600. lazy (the trial's own use): an address")
    ap.add_argument('--gen', help='trial: which generated code, gen_safe (default) or gen_abi')
    ap.add_argument('--autoplay', type=int, default=2, help='trial: 1 presses A and taps, 2 also wanders, 3 walks about')
    ap.add_argument('--seed', type=int)
    ap.add_argument('--speed', type=int, help='trial: percent. 200 gets further in the same time')
    ap.add_argument('--gpu-threads', type=int, default=4, help='trial: threads for the software renderer')
    ap.add_argument('--run', help='trial: a name of its own, so copies can run side by side')
    ap.add_argument('--save-from', help='trial: start from a copy of this save folder')
    ap.add_argument('--keep-save', action='store_true', help='trial: go on from the save the last run of this name left (tools/win/play_pc.py)')
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
    if not os.path.isdir(f'{ROOT}/games/{args.game}'):
        sys.exit(f'no games/{args.game}')
    if not os.path.exists(BIN + 'gcc.exe'):
        sys.exit('no toolchain: run tools/win/install_toolchain.sh once')
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
