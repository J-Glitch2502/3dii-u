"""Self-check for updater: finding a release in a canned GitHub answer, downloading from a fake server, refusing bad zips, and choosing,
handing over to and tidying away downloaded versions in a stand-in for the app's data folder. No network. Run: python builder/test_updater.py"""
import hashlib
import io
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import updater as up  # noqa: E402
assert 'ssl' not in sys.modules and 'urllib.request' not in sys.modules, 'app.py imports updater at every start: urllib and ssl load only when used'
assert "ssl" not in sys.modules and "urllib.request" not in sys.modules, "app.py imports updater at every start: urllib and ssl load only when used"

up.LOCK_WAIT, up.DIED = 0.3, 1  # short waits keep this quick
TOP, P = up.TOP, up.DOWNLOADS


def raises(f, *args, says='', **kw):
    try:
        f(*args, **kw)
    except up.UpdateError as e:
        assert says in str(e), (says, str(e))
        return str(e)
    raise AssertionError(f'{f.__name__} should have failed')


# tag_key
tags = ['builder-1.0-beta-2026-11-12.2', 'builder-1.1-2026-11-01', 'builder-1.0-beta-2026-11-12', 'builder-1.0-beta-2026-11-13',
        'builder-1.0-beta-2026-11-12.10', 'builder-1.0.1-beta-2026-11-02']
assert sorted(tags, key=up.tag_key) == ['builder-1.0-beta-2026-11-12', 'builder-1.0-beta-2026-11-12.2', 'builder-1.0-beta-2026-11-12.10',
                                        'builder-1.0-beta-2026-11-13', 'builder-1.0.1-beta-2026-11-02', 'builder-1.1-2026-11-01']
for t in ('deps-1', '', None, 'builder-1.0-beta-2026-11-12/../x', 'builder-x-2026-11-12', 'v1.0'):
    assert up.tag_key(t) is None, t
print('tag_key ok')


# newest
def release(tag, digest='sha256:' + 'a' * 64, url=None, draft=False, name='3Dii U Builder.zip', prerelease=False):
    return {'tag_name': tag, 'name': 'Title ' + tag, 'body': 'notes', 'draft': draft, 'prerelease': prerelease, 'assets': [
        {'name': 'notes.txt', 'size': 3, 'digest': digest, 'browser_download_url': P + tag + '/notes.txt'},
        {'name': name, 'size': 10, 'digest': digest, 'browser_download_url': url or P + tag + '/' + name}]}


LIST = [release('builder-1.0-beta-2026-11-22', prerelease=True), release('builder-1.0-beta-2026-11-21', name='Source code.zip'),
        release('builder-1.0-beta-2026-11-20', draft=True), release('deps-1'), release('builder-1.0-beta-2026-11-19', digest=None),
        release('builder-1.0-beta-2026-11-18', url='https://example.com/3Dii U Builder.zip'),
        release('builder-1.0-beta-2026-11-17', url='https://github.com/someone/else/releases/download/x/a.zip'),
        release('builder-1.0-beta-2026-11-16', digest='sha1:' + 'a' * 40),
        release('builder-1.0-beta-2026-11-12'), release('builder-1.0-beta-2026-11-14'), 'junk', {'tag_name': 'builder-1.0-2026-11-30'}]
r = up.newest(LIST)
assert r and r.tag == 'builder-1.0-beta-2026-11-14' and r.url.endswith('/3Dii U Builder.zip') and r.size == 10 and r.sha256 == 'a' * 64, r
assert r.title == 'Title builder-1.0-beta-2026-11-14' and r.notes == 'notes'
assert up.newest([]) is None and up.newest({'message': 'rate limited'}) is None and up.newest(LIST[:8]) is None
for name in ('3Dii U Builder.zip', '3Dii-U-Builder-1.0-win.ZIP', '3dii.u.builder.zip'):
    assert up.ASSET.fullmatch(name), name
for name in ('3Dii U Builder.zip.sig', 'other 3Dii U Builder.zip', '3DiiUBuilder.zip', '3Dii U Builder.7z'):
    assert not up.ASSET.fullmatch(name), name
print('newest ok')


# check
class Response(io.BytesIO):
    pass


asked = []


def serve(data):
    def opener(request, timeout):
        asked.append((request, timeout))
        return Response(data)
    return opener


api = serve(json.dumps(LIST).encode())
assert up.check('builder-1.0-beta-2026-11-12', api).tag == 'builder-1.0-beta-2026-11-14'
request, timeout = asked[0]
assert request.full_url == up.API == 'https://api.github.com/repos/J-Glitch2502/3dii-u/releases?per_page=30' and timeout == 8
assert request.get_header('User-agent') == '3DiiU-Builder' and request.get_header('Accept') == 'application/vnd.github+json'
assert request.data is None
assert up.check('builder-1.0-beta-2026-11-14', api) is None, 'up to date'
assert up.check('builder-1.0-beta-2026-11-14.1', api) is None, 'newer than any release'
assert up.check('', api).tag == 'builder-1.0-beta-2026-11-14', 'a development build takes any release'


def offline(request, timeout):
    raise urllib.error.URLError('no network')


assert up.check('', offline) is None
assert up.check('', serve(b'<html>not json')) is None
assert up.check('', serve(b'[' + b' ' * (3 << 20) + b']')) is None, 'an answer past 2 MB is not read'
print('check ok')

with tempfile.TemporaryDirectory() as tmp:
    # download
    data = os.urandom((3 << 20) + 5)
    good = up.Release('builder-1.0-beta-2026-11-14', 't', 'n', P + 'builder-1.0-beta-2026-11-14/a.zip', len(data), hashlib.sha256(data).hexdigest())
    dl = os.path.join(tmp, 'downloads')
    seen = []
    path = up.download(good, dl, progress=lambda d, t: seen.append((d, t, os.listdir(dl))), opener=serve(data))
    assert path == os.path.join(dl, good.tag + '.zip') and open(path, 'rb').read() == data
    assert os.listdir(dl) == [good.tag + '.zip'] and len(seen) == 4 and seen[-1][:2] == (len(data), len(data))
    assert seen[0][2] == [f'{good.tag}.{os.getpid()}.zip.part'], 'a part file of its own, so two windows never write the same one'
    os.remove(path)
    late = []
    path = up.download(good, dl, progress=lambda d, t: late.append(d), cancelled=lambda: len(late) == 4, opener=serve(data))
    assert path and open(path, 'rb').read() == data and os.listdir(dl) == [good.tag + '.zip'], 'a cancel after the last piece keeps it'
    os.remove(path)
    for bad, says in ((up.Release(**{**good.__dict__, 'sha256': '0' * 64}), 'doesn\'t match'),
                      (up.Release(**{**good.__dict__, 'size': len(data) + 1}), 'doesn\'t match'),
                      (up.Release(**{**good.__dict__, 'size': len(data) - 1}), 'bigger')):
        raises(up.download, bad, dl, opener=serve(data), says=says)
        assert os.listdir(dl) == [], os.listdir(dl)
    pieces = []
    assert up.download(good, dl, progress=lambda d, t: pieces.append(d), cancelled=lambda: len(pieces) >= 2, opener=serve(data)) is None
    assert len(pieces) == 2 and os.listdir(dl) == []

    def stops(request, timeout):
        class Cut(Response):
            def read(self, n=-1):
                if self.tell() > 0:
                    raise ConnectionResetError('reset')
                return super().read(n)
        return Cut(data)
    raises(up.download, good, dl, opener=stops, says='stopped')
    assert os.listdir(dl) == []
    asked.clear()
    for url in ('https://example.com/a.zip', 'https://github.com/J-Glitch2502/3dii-u.evil/releases/download/x/a.zip'):
        raises(up.download, up.Release(**{**good.__dict__, 'url': url}), dl, opener=serve(data), says='doesn\'t come from')
    raises(up.download, up.Release(**{**good.__dict__, 'tag': '../x'}), dl, opener=serve(data))
    assert asked == [] and os.listdir(dl) == []
    raises(up.need_room, tmp, 1 << 60, 'the update', says='free space')
    print('download ok')

    # unpack
    def tree(folder, stamp='1', text='new', extra=0, notices=True, tag='builder-1.0-beta-2026-11-14'):
        """a small stand-in for the app: files.txt counts its files as make_app.py does"""
        files = {up.EXE: text, '_internal/stamp.txt': stamp, 'engine/stamp.txt': stamp, 'engine/tools/build.py': text,
                 'LICENSE.txt': 'licence', '_internal/lib.dll': text, '_internal/release.txt': tag}
        if notices:
            files['Third-party notices.txt'] = text
        for name, body in files.items():
            os.makedirs(os.path.dirname(os.path.join(folder, name)), exist_ok=True)
            with open(os.path.join(folder, name), 'w') as f:
                f.write(body)
        with open(os.path.join(folder, '_internal', 'files.txt'), 'w') as f:
            f.write(str(len(files) + 1 + extra))
        return folder

    def zipped(src, name, also=()):
        path = os.path.join(tmp, name + '.zip')
        with zipfile.ZipFile(path, 'w') as z:
            for d, _, fs in os.walk(src):
                for f in fs:
                    z.write(os.path.join(d, f), os.path.join(TOP, os.path.relpath(os.path.join(d, f), src)).replace('\\', '/'))
            for member in also:
                z.writestr(member, 'evil')
        return path

    tag = 'builder-1.0-beta-2026-11-14'
    out = os.path.join(tmp, 'unpacked')
    new = tree(os.path.join(tmp, 'src_new'), stamp='2', notices=False)
    app = up.unpack(zipped(new, tag), out)
    assert app == os.path.join(out, tag, TOP) and open(os.path.join(app, up.EXE)).read() == 'new'
    for also, says in ((['3Dii U Builder/../evil.txt'], 'outside'), (['/evil.txt'], 'outside'), (['C:/evil.txt'], 'outside'),
                       (['3Dii U Builder\\..\\..\\evil.txt'], 'outside'), (['Other/evil.txt'], 'outside'), (['evil.txt'], 'outside')):
        raises(up.unpack, zipped(new, 'builder-1.0-beta-2026-11-15', also), out, says=says)
        assert not os.path.exists(os.path.join(out, 'builder-1.0-beta-2026-11-15')) and not os.path.exists(os.path.join(tmp, 'evil.txt'))
    raises(up.unpack, zipped(tree(os.path.join(tmp, 'few'), extra=5), 'builder-1.0-beta-2026-11-16'), out, says='missing')
    tree(os.path.join(tmp, 'mixed'))
    with open(os.path.join(tmp, 'mixed', 'engine', 'stamp.txt'), 'w') as f:
        f.write('other')
    raises(up.unpack, zipped(os.path.join(tmp, 'mixed'), 'builder-1.0-beta-2026-11-17'), out, says='two versions')
    with open(os.path.join(tmp, 'builder-1.0-beta-2026-11-18.zip'), 'wb') as f:
        f.write(b'not a zip')
    raises(up.unpack, os.path.join(tmp, 'builder-1.0-beta-2026-11-18.zip'), out, says='couldn\'t be unpacked')
    raises(up.unpack, zipped(new, 'deps-1'), out)
    raises(up.unpack, zipped(new, 'builder-1.0-beta-2026-11-20'), out, says='labelled as another version')
    unlabelled = tree(os.path.join(tmp, 'unlabelled'), tag='builder-1.0-beta-2026-11-21')
    os.remove(os.path.join(unlabelled, '_internal', 'release.txt'))
    with open(os.path.join(unlabelled, '_internal', 'files.txt'), 'w') as f:
        f.write('1')
    raises(up.unpack, zipped(unlabelled, 'builder-1.0-beta-2026-11-21'), out, says='labelled as another version')
    assert sorted(os.listdir(out)) == [tag], os.listdir(out)
    # the same version again, while a window of it runs: refused, and the running one stays whole
    held = open(os.path.join(app, '_internal', 'lib.dll'))
    try:
        raises(up.unpack, zipped(new, tag), out, says='in use')
        assert sorted(os.listdir(out)) == [tag] and open(os.path.join(app, up.EXE)).read() == 'new', os.listdir(out)
    finally:
        held.close()
    assert up.unpack(zipped(new, tag), out) == app and sorted(os.listdir(out)) == [tag], 'once closed, it is replaced'
    # a later version's zip may hold an entry this version has never heard of: in a fresh unpack it counts like any other
    later = tree(os.path.join(tmp, 'later'), stamp='3', tag='builder-1.0-beta-2026-11-19')
    with open(os.path.join(later, 'Something new.txt'), 'w') as f:
        f.write('new')
    with open(os.path.join(later, '_internal', 'files.txt'), 'w') as f:
        f.write(str(sum(len(fs) for _, _, fs in os.walk(later))))
    assert os.path.isfile(os.path.join(up.unpack(zipped(later, 'builder-1.0-beta-2026-11-19'), os.path.join(tmp, 'unpacked_later')), 'Something new.txt'))
    print('unpack ok')

    # activate and the launch record: in a stand-in for the app's data folder
    os.environ['LOCALAPPDATA'] = os.path.join(tmp, 'local')
    home = up.home()
    assert home == os.path.join(tmp, 'local', '3Dii U Builder', 'app')
    OLD, NEW = 'builder-1.0-beta-2026-11-12', 'builder-1.0-beta-2026-11-14'
    launch = os.path.join(home, 'launch.json')

    def record():
        with open(launch) as f:
            return json.load(f)

    def put(**fields):
        if os.path.exists(launch):
            os.chmod(launch, stat.S_IWRITE)
        with open(launch, 'w') as f:
            json.dump({'format': 1, 'tag': NEW, 'exe': os.path.join(NEW, TOP, up.EXE), 'state': 'new', 'since': time.time(), **fields}, f)

    os.makedirs(home)
    zip_new = os.path.join(home, NEW + '.zip')
    shutil.copy(zipped(new, NEW), zip_new)
    v = up.unpack(zip_new, home)
    exe = os.path.join(v, up.EXE)
    up.activate(v)
    r = record()
    assert r['format'] == 1 and r['tag'] == NEW and r['state'] == 'new' and os.path.join(home, r['exe']) == exe and abs(r['since'] - time.time()) < 60
    assert sorted(os.listdir(home)) == sorted(['launch.json', 'launch.lock', NEW]), 'the zip goes, and no temporary file stays'
    raises(up.activate, app, says='isn\'t a downloaded update')
    raises(up.activate, os.path.dirname(v), says='isn\'t a downloaded update')
    assert record() == r
    print('activate ok')

    # current: the stub's state machine
    assert up.current(OLD) == exe and record()['state'] == 'trying', 'new: tried'
    before = open(launch).read()
    assert up.current(OLD) == exe and open(launch).read() == before, 'trying, fresh: another start under way, the record as it was'
    for since in (time.time() - up.TRYING - 1, time.time() + up.TRYING + 60, 'soon', None):
        put(state='trying', since=since)
        assert up.current(OLD) is None and record()['state'] == 'failed', since
    assert up.current(OLD) is None and record()['state'] == 'failed', 'failed stays failed'
    put(state='ok')
    assert up.current(OLD) == exe and record()['state'] == 'trying', 'ok: tried again'
    put()
    for own in (NEW, 'builder-1.0-beta-2026-11-15', '', 'deps-1'):
        assert up.current(own) is None and record()['state'] == 'new', own  # equal, older, development builds
    real_running = up.running_from
    up.running_from = lambda: v
    assert up.current(OLD) is None, 'never to the version this process is'
    up.running_from = real_running
    for bad in ('..\\' + os.path.join(NEW, TOP, up.EXE), os.path.join(NEW, '..', NEW, TOP, up.EXE), exe, 'C:' + up.EXE, '\\' + up.EXE, '', 5, None,
                os.path.join('builder-1.0-beta-2026-11-16', TOP, up.EXE)):
        put(exe=bad)
        assert up.current(OLD) is None and record()['state'] == 'new', bad
    for fields in ({'format': 2}, {'format': '1'}, {'tag': 'deps-1'}, {'tag': None}, {'state': 'odd'}):
        put(**fields)
        assert up.current(OLD) is None, fields
    for garbage in ('garbage', '[1]', '', '{"format": 1'):
        with open(launch, 'w') as f:
            f.write(garbage)
        assert up.current(OLD) is None, garbage
    put()
    os.chmod(launch, stat.S_IREAD)
    assert up.current(OLD) is None, 'never handed over without being recorded'
    os.chmod(launch, stat.S_IWRITE)
    assert record()['state'] == 'new'
    print('current ok')

    # confirm, set_aside
    real_own = up.own_tag
    put(state='trying')
    up.confirm()
    assert record()['state'] == 'trying', 'the app the person unzipped confirms nothing'
    up.running_from, up.own_tag = (lambda: v), (lambda: OLD)
    up.confirm()
    assert record()['state'] == 'trying', 'nor another version'
    up.own_tag = lambda: NEW
    up.confirm()
    assert record()['state'] == 'ok'
    up.confirm()
    assert record()['state'] == 'ok'
    assert up.set_aside() and record()['state'] == 'failed' and up.current(OLD) is None and up.failed_tags() == [NEW]
    up.own_tag = lambda: 'builder-1.0-beta-2026-11-20'
    put()
    assert not up.set_aside() and record()['state'] == 'new', 'a newer version\'s record is not this one\'s to judge'
    os.remove(launch)
    assert not up.set_aside()
    up.running_from, up.own_tag = real_running, real_own
    print('confirm, set_aside ok')

    # the failed list: a release that failed to start isn't offered again by itself
    assert up.failed_tags() == []
    put(state='ok')
    up.running_from, up.own_tag = (lambda: v), (lambda: NEW)
    assert up.set_aside() and up.failed_tags() == [NEW]
    up.running_from, up.own_tag = real_running, real_own
    assert up.check('builder-1.0-beta-2026-11-10', api).tag == OLD, 'the next newer release instead'
    assert up.check(OLD, api) is None, 'or nothing'
    assert up.check(OLD, api, skip_failed=False).tag == NEW, 'unless the person asks'
    other = 'builder-1.0-beta-2026-11-19'
    shutil.copy(zipped(later, other), os.path.join(home, other + '.zip'))
    up.activate(up.unpack(os.path.join(home, other + '.zip'), home))
    assert record()['tag'] == other and up.failed_tags() == [NEW], 'kept when another version is activated'
    up.activate(v)
    assert record()['tag'] == NEW and record()['state'] == 'new' and up.failed_tags() == [], 'activating it again is a deliberate retry'
    up.remove(os.path.join(home, other))
    print('failed list ok')

    # the record lock: while another process holds it, nothing is written, and each call says so within its wait
    put()
    before = open(launch).read()
    with up.record_lock() as held:
        assert held
        t = time.monotonic()
        assert up.current(OLD) is None and not up.set_aside() and not up.confirm()
        raises(up.activate, v, says='busy')
        assert time.monotonic() - t < 4 * up.LOCK_WAIT + 2
    assert open(launch).read() == before
    assert up.current(OLD) == exe and record()['state'] == 'trying', 'and it works once the lock is free'
    print('record lock ok')

    # started: confirms, and tidies without holding up the window
    up.running_from, up.own_tag = (lambda: v), (lambda: NEW)
    real_tidy, tidied = up.tidy, threading.Event()
    up.tidy = lambda: (time.sleep(0.5), tidied.set())
    t = time.monotonic()
    up.started()
    assert time.monotonic() - t < 0.4 and record()['state'] == 'ok' and not tidied.is_set()
    assert tidied.wait(5)
    up.tidy = real_tidy
    print('started ok')

    # hold_version: a running downloaded version keeps its folder from being cleared
    lock = os.path.join(os.path.dirname(v), f'in-use-{os.getpid()}.lock')
    open(os.path.join(os.path.dirname(v), 'in-use-1.lock'), 'w').close()  # left by a process that has ended
    up.hold_version()
    assert up.held_version and os.path.isfile(lock) and not os.path.exists(os.path.join(os.path.dirname(v), 'in-use-1.lock'))
    assert not up.discard(os.path.dirname(v), NEW) and os.path.isfile(exe), 'a running version is left alone'
    up.held_version.close()
    up.held_version = None
    other_lock = os.path.join(os.path.dirname(v), 'in-use-other.lock')
    holder = subprocess.Popen([sys.executable, '-c', f'import time; f = open({other_lock!r}, "a"); time.sleep(30)'])
    try:
        for _ in range(200):
            if os.path.exists(other_lock):
                break
            time.sleep(0.05)
        assert not up.discard(os.path.dirname(v), NEW) and os.path.isfile(exe), 'nor one another process runs'
    finally:
        holder.kill()
        holder.wait()
    os.remove(other_lock)
    up.running_from = lambda: app  # the app the person unzipped
    outside = sorted(os.listdir(app))
    up.hold_version()
    assert up.held_version is None and sorted(os.listdir(app)) == outside, 'it holds nothing and writes nothing'
    up.running_from, up.own_tag = real_running, real_own
    print('hold_version ok')

    # hand_over: a version that ends at once with an error has failed, and the app opens as itself
    put(state='trying')
    assert up.hand_over(sys.executable, ['-c', 'import sys; sys.exit(3)']) is False and record()['state'] == 'failed' and up.failed_tags() == [NEW]
    put(state='trying')
    assert up.hand_over(sys.executable, ['-c', 'pass']) is True and record()['state'] == 'trying', 'closed at once by the person: no failure'
    put(state='trying')
    assert up.hand_over(sys.executable, ['-c', 'import time; time.sleep(5)']) is True and record()['state'] == 'trying'
    print('hand_over exit ok')

    # who may hand over
    assert up.may_hand_over([], {}) and up.may_hand_over(['some.3ds'], {'PATH': 'x'})
    assert not up.may_hand_over(['--pipeline', 'x'], {}) and not up.may_hand_over(['tools/win/build3ds.py'], {})
    assert not up.may_hand_over([], {'THREEDII_HANDED_OVER': '1'}), 'one hop only'
    print('may_hand_over ok')

    # tidy
    put()
    ro = os.path.join(tree(os.path.join(home, 'builder-1.0-beta-2026-11-13', TOP)), '_internal', 'lib.dll')
    os.chmod(ro, stat.S_IREAD)
    tree(os.path.join(home, 'builder-1.0-beta-2026-11-10', TOP))
    open_old = open(os.path.join(home, 'builder-1.0-beta-2026-11-10', TOP, '_internal', 'lib.dll'))  # a window of it still runs
    for name in (OLD + '.zip', 'builder-1.0-beta-2026-11-15.999.zip.part', 'launch.json.999.tmp', 'notes.txt', 'deps-1.zip'):
        open(os.path.join(home, name), 'w').close()
    for name in ('my stuff', 'builder-notes', 'trash-builder-1.0-beta-2026-11-09-999', 'trash-builder-1.0-beta-2026-11-08-998',
                 NEW + '.unpacking-999', NEW + '.unpacking-998'):
        os.makedirs(os.path.join(home, name))

    def aged(name):
        """as if it had lain there two days"""
        os.utime(os.path.join(home, name), (time.time() - 2 * 86400,) * 2)

    arriving = 'builder-1.0-beta-2026-11-19.4242.zip.part'  # newer than the current version and fresh: another window may be downloading it
    open(os.path.join(home, arriving), 'w').close()
    for name in ('builder-1.0-beta-2026-11-15.999.zip.part', 'launch.json.999.tmp', 'trash-builder-1.0-beta-2026-11-09-999', NEW + '.unpacking-999'):
        aged(name)  # abandoned
    try:
        up.tidy()
        assert sorted(os.listdir(home)) == sorted(['launch.json', 'launch.lock', NEW, arriving, 'notes.txt', 'deps-1.zip', 'my stuff', 'builder-notes',
                                                   'builder-1.0-beta-2026-11-10', 'trash-builder-1.0-beta-2026-11-08-998', NEW + '.unpacking-998']), os.listdir(home)
        assert open_old.read() == 'new', 'a version in use is left whole'
    finally:
        open_old.close()
    up.tidy()
    assert not os.path.exists(os.path.join(home, 'builder-1.0-beta-2026-11-10')), 'and cleared once it is closed'
    mine = tree(os.path.join(home, 'builder-1.0-beta-2026-11-18', TOP))
    up.running_from = lambda: mine
    up.tidy()
    up.running_from = real_running
    assert os.path.isdir(mine) and os.path.isdir(v), 'the version this process is stays, and so does the current one'
    os.remove(launch)
    up.tidy()
    assert os.path.isdir(v) and os.path.isdir(mine), 'with nothing known to be newer, what is fresh stays'
    for name in (NEW, 'builder-1.0-beta-2026-11-18', arriving):
        aged(name)
    up.tidy()
    assert not os.path.exists(v) and not os.path.isdir(mine) and not os.path.exists(os.path.join(home, arriving)) and os.path.isdir(os.path.join(home, 'my stuff'))
    assert not [n for n in os.listdir(home) if n.startswith('trash-builder-1.0-beta-2026-11-1')], os.listdir(home)
    # an app newer than what was downloaded before it came: that version goes
    tree(os.path.join(home, NEW, TOP))
    put()
    up.own_tag = lambda: 'builder-1.0-beta-2026-11-20'
    up.tidy()
    up.own_tag = real_own
    assert not os.path.exists(os.path.join(home, NEW))
    os.environ['LOCALAPPDATA'] = os.path.join(tmp, 'nowhere')
    up.tidy()
    assert up.current(OLD) is None and not up.set_aside()
    up.confirm()
    print('tidy ok')

    # fresh_env, hand_over
    os.environ.update({'THREEDII_ROOT': 'x', '_PYI_APPLICATION_HOME_DIR': 'y', '_MEIPASS2': 'z', 'THREEDII_HANDED_OVER': '1'})
    env = up.fresh_env()
    assert env['PYINSTALLER_RESET_ENVIRONMENT'] == '1' and 'PATH' in {k.upper() for k in env}
    assert not [k for k in env if k.upper().startswith(('THREEDII_', '_PYI_')) or k.upper() == '_MEIPASS2'], env.keys()
    child, said = os.path.join(tmp, 'child.py'), os.path.join(tmp, 'child.txt')
    with open(child, 'w') as f:
        f.write('import os, sys\n'
                'env = {k.upper(): v for k, v in os.environ.items()}\n'
                'ok = env.get("PYINSTALLER_RESET_ENVIRONMENT") == "1" and env.get("THREEDII_HANDED_OVER") == "1" and "THREEDII_ROOT" not in env\n'
                'open(sys.argv[1] + ".part", "w").write("ok" if ok and sys.argv[2:] == ["a b"] else "bad")\n'
                'os.replace(sys.argv[1] + ".part", sys.argv[1])\n'
                'import time; time.sleep(3)\n')  # a window stays up
    assert up.hand_over(sys.executable, [child, said, 'a b']) is True
    for _ in range(200):
        if os.path.exists(said):
            break
        time.sleep(0.05)
    assert open(said).read() == 'ok', 'started marked as handed over, with a fresh environment'
    assert up.hand_over(os.path.join(tmp, 'nowhere.exe'), []) is False
    for k in ('THREEDII_ROOT', '_PYI_APPLICATION_HOME_DIR', '_MEIPASS2', 'THREEDII_HANDED_OVER'):
        del os.environ[k]
    print('hand_over ok')

assert up.own_tag() == ''
print('updater: ok')
