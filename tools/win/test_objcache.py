"""Self-check of the cache of compiled code (objcache.py, build3ds.compile_all): a small C file compiled twice into two folders comes the second
time from the cache, byte-identical, with its dependency file, and isn't stale afterwards; a changed header makes a new object.
Run: python tools/win/test_objcache.py (needs devkitPPC, as builds do)."""
import filecmp
import importlib.util
import os
import shutil
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('b3', os.path.join(HERE, 'build3ds.py'))
b3 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b3)

tmp = tempfile.mkdtemp(prefix='objcache_test_')
try:
    os.environ['THREEDII_OBJCACHE'] = os.path.join(tmp, 'cache')
    src, head = os.path.join(tmp, 'x.c'), os.path.join(tmp, 'x.h')
    open(head, 'w').write('#define N 3\n')
    open(src, 'w').write('#include "x.h"\nint f(int a) { return a * N; }\n')
    flags = [b3.BIN + 'gcc.exe', '-O2', '-mcpu=750', f'-I{tmp}']
    objs = []
    for n in (1, 2, 3):
        if n == 3:
            open(head, 'w').write('#define N 5\n')  # what the preprocessor sees changed: a new object
        d = os.path.join(tmp, f'r{n}')
        os.makedirs(d)
        obj = os.path.join(d, 'x.o')
        b3.compile_all([('x', src, obj, flags)], 1, f'round {n}')
        assert os.path.isfile(obj + '.d') and not b3.stale(obj, src, flags, False), n
        objs.append(obj)
    assert filecmp.cmp(objs[0], objs[1], shallow=False), 'the cached object differs'
    assert not filecmp.cmp(objs[0], objs[2], shallow=False), 'a changed header reused the old object'
    print('cache of compiled code: ok')
finally:
    shutil.rmtree(tmp, ignore_errors=True)
