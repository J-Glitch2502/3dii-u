"""Self-check for app.py's compiled-cache reset: python builder/test_app.py"""
import importlib.util
import os
import shutil
import tempfile

spec = importlib.util.spec_from_file_location('app', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app.py'))
app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(app)

base = tempfile.mkdtemp()
engine, work = os.path.join(base, 'engine'), os.path.join(base, 'work')
cache = os.path.join(work, 'build_win_builder' if os.name == 'nt' else 'build_linux_builder')


def compiled():
    os.makedirs(cache, exist_ok=True)
    open(os.path.join(cache, 'x.o'), 'w').close()


def stamp(s):
    with open(os.path.join(engine, 'stamp.txt'), 'w') as f:
        f.write(s)


try:
    os.makedirs(engine)
    compiled()
    app.fresh_cache(engine, work)
    assert os.path.exists(os.path.join(cache, 'x.o')), 'an engine without a stamp leaves the cache alone'
    stamp('1')
    app.fresh_cache(engine, work)
    assert not os.path.exists(cache), 'a new engine starts the cache over'
    compiled()
    app.fresh_cache(engine, work)
    assert os.path.exists(os.path.join(cache, 'x.o')), 'the same engine keeps it'
    stamp('2')
    app.fresh_cache(engine, work)
    assert not os.path.exists(cache), 'another engine starts it over again'
    print('fresh_cache ok')
finally:
    shutil.rmtree(base)
