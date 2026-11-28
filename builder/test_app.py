"""Self-check for app.py's compiled-cache reset and its output's encoding, and for make_app.py's release tag and its check that a Complete game
has its confirmed shader list: python builder/test_app.py"""
import importlib.util
import hashlib
from pathlib import Path
import os
import shutil
import tempfile
import types
from unittest import mock

spec = importlib.util.spec_from_file_location('app', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app.py'))
app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(app)

base = tempfile.mkdtemp(prefix='app-cache-safety-')
engine, work = os.path.join(base, 'engine'), os.path.join(base, 'work')
cache = os.path.join(work, 'build_win_builder')


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
    compiled()
    stamp('3')
    with mock.patch.object(shutil, 'rmtree', return_value=None):  # Windows can leave locked objects behind
        try:
            app.fresh_cache(engine, work)
        except OSError:
            pass
        else:
            raise AssertionError('a failed cache cleanup allowed the build to use old objects')
    with open(os.path.join(work, 'engine_stamp.txt')) as f:
        assert f.read() == '2', 'a failed cache cleanup advanced the engine stamp'
    app.fresh_cache(engine, work)
    assert not os.path.exists(cache), 'cleanup was not retried after the lock was released'
    other = os.path.join(base, 'other-build-folder')
    other_cache = os.path.join(other, 'build_win_builder')
    os.makedirs(other_cache)
    open(os.path.join(other_cache, 'old.o'), 'w').close()
    with open(os.path.join(other, 'engine_stamp.txt'), 'w') as f:
        f.write('older engine')
    with mock.patch.dict(os.environ, {'THREEDII_ROOT': engine, 'THREEDII_WORK': work, 'THREEDII_BUILD': other}), \
            mock.patch.object(app.sys, 'argv', ['Builder.exe', '--pipeline', '--game', 'unused']):
        app.use_engine(engine)
    assert os.path.exists(other_cache), 'dispatch must leave refresh to the locked pipeline'
    print('fresh_cache ok')
    # Cache refresh must reject aliases before deleting anything or publishing its stamp.
    protected = Path(base) / 'synthetic-nand' / 'title.tik'
    protected.parent.mkdir()
    protected.write_bytes(b'SYNTHETIC TICKET - must survive cache refresh')

    def ticket_state():
        st = protected.stat()
        data = protected.read_bytes()
        return data, hashlib.sha256(data).digest(), st.st_mtime_ns, st.st_nlink, st.st_ino

    def ordinary(name):
        folder = Path(base) / name
        (folder / 'build_win_builder' / 'nested').mkdir(parents=True)
        (folder / 'build_win_builder' / 'old.o').write_bytes(b'old object')
        return folder

    def rejected(folder):
        before = ticket_state()
        with mock.patch.object(shutil, 'rmtree', side_effect=AssertionError('unsafe cache reached deletion')):
            try:
                app.fresh_cache(engine, str(folder))
            except (OSError, ValueError):
                pass
            else:
                raise AssertionError('unsafe cache path was accepted')
        assert ticket_state() == before, 'ticket bytes/hash/mtime/link count/file ID changed'

    linked = ordinary('hardlinked-stamp')
    os.link(protected, linked / 'engine_stamp.txt')
    try:
        rejected(linked)
        assert (linked / 'build_win_builder' / 'old.o').read_bytes() == b'old object'
    finally:
        (linked / 'engine_stamp.txt').unlink()

    linked = ordinary('nested-hardlink')
    alias = linked / 'build_win_builder' / 'nested' / 'cached.o'
    os.link(protected, alias)
    try:
        rejected(linked)
    finally:
        alias.unlink()

    linked = ordinary('symlinked-stamp')
    alias = linked / 'engine_stamp.txt'
    try:
        alias.symlink_to(protected)
    except OSError as e:
        if os.name != 'nt' or getattr(e, 'winerror', None) != 1314:
            raise
        print('file symlink unavailable: Windows did not grant symbolic-link privilege')
    else:
        try:
            rejected(linked)
        finally:
            alias.unlink()

    def junction(alias, target):
        if os.name == 'nt':
            import subprocess
            r = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(alias), str(target)], capture_output=True)
            assert r.returncode == 0, r.stderr
        else:
            alias.symlink_to(target, target_is_directory=True)

    for nested in (False, True):
        folder = ordinary('junction-' + str(nested))
        alias = folder / 'build_win_builder' / 'nested' / 'outside' if nested else Path(base) / 'alias-work'
        junction(alias, protected.parent if nested else folder)
        try:
            rejected(folder if nested else alias)
            if not nested:
                rejected(alias / 'child')  # a reparse ancestor is unsafe even when the final path does not exist
        finally:
            alias.rmdir() if os.name == 'nt' else alias.unlink()

    forbidden = Path(base) / 'nand' / 'build'
    rejected(forbidden)
    assert not forbidden.exists(), 'a protected build folder was created before rejection'
    folder = ordinary('directory-stamp')
    (folder / 'engine_stamp.txt').mkdir()
    rejected(folder)

    # A failed atomic publication keeps the old stamp and remains retryable.
    folder = ordinary('publication-failure')
    (folder / 'engine_stamp.txt').write_text('old engine')
    with mock.patch.object(os, 'replace', side_effect=OSError('injected publication failure')):
        try:
            app.fresh_cache(engine, str(folder))
        except OSError:
            pass
        else:
            raise AssertionError('stamp publication failure was ignored')
    assert (folder / 'engine_stamp.txt').read_text() == 'old engine'
    assert sorted(p.name for p in folder.iterdir()) == ['engine_stamp.txt'], 'temporary stamp leaked'
    app.fresh_cache(engine, str(folder))
    assert (folder / 'engine_stamp.txt').read_text() == '3'
    assert (folder / 'engine_stamp.txt').stat().st_nlink == 1
    print('cache aliases and atomic stamp: ok')

    # what a build prints reaches the window as UTF-8 whatever the PC's code page: a name with ★ or ™ used to end the build
    import subprocess
    import sys
    script = os.path.join(base, 'say.py')
    with open(script, 'w', encoding='utf-8') as f:
        f.write('print("Kumamon\\u2605 SUPER STREET FIGHTER\\u2122 \\u30df\\u30a4")\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith('PYTHON')}
    r = subprocess.run([sys.executable, '-X', 'utf8=0', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app.py'), script], capture_output=True, env=env)
    assert r.returncode == 0 and r.stdout.decode('utf-8').strip() == 'Kumamon★ SUPER STREET FIGHTER™ ミイ', (r.returncode, r.stdout, r.stderr)
    print('output encoding ok')
    # a build to publish is given its release's tag, and only one of this version's
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_app
    tag = 'builder-1.0-beta-2026-11-13'
    assert make_app.release_tag(['make_app.py'], '1.0') == '' and make_app.release_tag(['make_app.py', '--release', tag], '1.0') == tag
    for bad in (['--release'], ['--release', 'deps-1'], ['--release', 'builder-2.0-beta-2026-11-13'], ['--release', '1.0']):
        try:
            make_app.release_tag(['make_app.py'] + bad, '1.0')
        except SystemExit:
            continue
        raise AssertionError(f'{bad} was taken as a release tag')
    print('release tag ok')
    # Collection must not prefer an unrelated tool's DLLs over Windows' own ICU.
    # A failed collector must also restore PATH for the remaining build tools.
    class Collected(Exception):
        pass

    def collect(args):
        assert os.environ['PATH'].split(os.pathsep)[0] == os.path.join(os.environ['WINDIR'], 'System32')
        raise Collected

    installer = types.ModuleType('PyInstaller')
    installer.__main__ = types.ModuleType('PyInstaller.__main__')
    installer.__main__.run = collect
    with mock.patch.dict(sys.modules, {'PyInstaller': installer, 'PyInstaller.__main__': installer.__main__,
                                      'gui': types.SimpleNamespace(VERSION='1.0')}), \
            mock.patch.dict(os.environ, {'PATH': 'unrelated-tools', 'WINDIR': os.environ.get('WINDIR', os.path.join(base, 'Windows'))}), \
            mock.patch.object(make_app, 'make_icon'), mock.patch.object(make_app, 'version_resource'), \
            mock.patch.object(make_app, 'unconfirmed', return_value=[]), \
            mock.patch.object(make_app.os, 'makedirs'):
        try:
            make_app.main()
        except Collected:
            pass
        else:
            raise AssertionError('did not reach the collector')
        assert os.environ['PATH'] == 'unrelated-tools'
    print('packaging DLL search order ok')
    # a game called Complete must have its confirmed shader list before the app is made; the list's notes and other games don't count
    games = os.path.join(base, 'games')
    compat = {'_about': 'notes', '_wiiu': {'3': {'status': 'Plays'}}, '_cemu': {'4': {'name': 'Cemu only'}},
              '1': {'slug': 'done', 'status': 'Complete'}, '2': {'slug': 'plays', 'status': 'Plays'}}
    assert make_app.unconfirmed(compat, games) == ['done']
    listed = os.path.join(games, 'done', 'shader_logs', 'confirmed.bin')
    os.makedirs(os.path.dirname(listed))
    open(listed, 'wb').close()
    assert make_app.unconfirmed(compat, games) == ['done'], 'an empty list is no list'
    states = b'\x09\x00\x00\x00state one' + b'\x09\x00\x00\x00state two'
    with open(listed, 'wb') as f:
        f.write(states)
    assert make_app.unconfirmed(compat, games) == []
    make_app.bundle_logs(os.path.join(games, 'done'), os.path.join(base, 'shipped'))  # and the app ships it as it is
    with open(os.path.join(base, 'shipped', 'shader_logs', 'confirmed.bin'), 'rb') as f:
        assert f.read() == states
    print('confirmed lists ok')
finally:
    resolved = Path(base).resolve()
    assert resolved.parent == Path(tempfile.gettempdir()).resolve() and resolved.name.startswith('app-cache-safety-')
    shutil.rmtree(base)
