"""Compiled objects kept by what went into them, as ccache does: the compile line without its paths, the compiler, and the source after the
preprocessor (every header it read). A game built again, by a new builder app or with an update, then compiles only the files whose code changed:
the app deletes a game's work files after each build, and a new app's engine starts its work folder over (app.fresh_cache), so without this every
build compiled all of a game's code again (Zelda: 110 files, 90 s of its 254 s).

The cache lives in <work>/../objcache, outside what fresh_cache wipes, and is trimmed to MAX_BYTES, least recently used first. Trouble with the
cache (a file another build trimmed or holds open, a full disk) never fails a build: the object is then just compiled, or not kept."""
import hashlib
import os
import shutil
import subprocess
import time

MAX_BYTES = 2 << 30  # disk space is tight on the PCs this runs on


def _compiler_id(exe):
    st = os.stat(exe)
    return f'{os.path.basename(exe)} {st.st_size} {int(st.st_mtime)}'


def key(argv, src, deps):
    """the cache key for compiling src with argv (the compiler first, no -c/-o), or None if the preprocessor fails (the compile reports it).
    The same pass writes the dependency file the compile would (deps), so a fetched object needs no compile at all. Line markers stay in when the
    flags ask for debug information, whose line tables come from them."""
    flags = [a for a in argv[1:] if not a.startswith(('-I', '-isystem', '-MF', '-MMD')) and not os.path.isabs(a)]
    marks = [] if '-g' in argv else ['-P']
    pp = subprocess.run([argv[0]] + [a for a in argv[1:] if a != '-MMD'] + ['-E'] + marks + ['-MMD', '-MF', deps, src], stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL)
    if pp.returncode:
        return None
    h = hashlib.sha256()
    h.update(_compiler_id(argv[0]).encode())
    h.update('\0'.join(flags).encode())
    h.update(pp.stdout)
    return h.hexdigest()


def _path(cache, k):
    return os.path.join(cache, k[:2], k + '.o')


def fetch(cache, k, obj):
    """the cached object for key k at obj; False if there's none, or it went while being read"""
    p = _path(cache, k)
    try:
        shutil.copyfile(p, obj)
        os.utime(p)  # recently used
        return True
    except OSError:
        return False


def store(cache, k, obj):
    p = _path(cache, k)
    tmp = f'{p}.{os.getpid()}.{time.time_ns()}.part'
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        shutil.copyfile(obj, tmp)
        os.replace(tmp, p)  # whole or not at all, if two builds store the same object
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass


def trim(cache, max_bytes=MAX_BYTES):
    """drops the least recently used objects until the cache is under max_bytes"""
    files = []
    for d, _, names in os.walk(cache):
        for n in names:
            if not n.endswith('.o'):
                continue
            p = os.path.join(d, n)
            try:
                st = os.stat(p)
            except OSError:
                continue
            files.append((st.st_mtime, st.st_size, p))
    total = sum(s for _, s, _ in files)
    for _, s, p in sorted(files):
        if total <= max_bytes:
            break
        try:
            os.remove(p)
            total -= s
        except OSError:
            pass
