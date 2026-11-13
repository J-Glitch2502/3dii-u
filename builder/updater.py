"""Updates the standalone app from its GitHub releases: finds a newer release, downloads its zip and checks it against the SHA-256 GitHub
publishes, and unpacks it into the app's own data folder (home()). The folder the person unzipped the app into is never written to: the app
there hands over to the newest unpacked version when its window starts (current(), hand_over()), and a launch record (launch.json) lets it
stop doing so when that version never came up. Asking GitHub sends nothing about the person or the PC. Everything here fails with UpdateError,
a message a person can read; a check that can't reach GitHub just finds nothing.

What a later release must keep, for apps already out there to find it, take it and hand over to it:
- its tag: builder-<version>[-<label>]-<yyyy>-<mm>-<dd>[.<n>] (TAG); a published release, neither draft nor prerelease, whose .zip asset
  is named like 3Dii U Builder*.zip (ASSET) and carries GitHub's sha256 digest
- the zip's one top folder "3Dii U Builder", holding "3Dii U Builder.exe"
- _internal/files.txt (the number of files), _internal/stamp.txt equal to engine/stamp.txt, and _internal/release.txt holding the tag
- launch.json format 1: tag, exe relative to home(), state, since, failed; and launch.lock as the lock around it"""
import contextlib
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import threading
import time
import zipfile
from dataclasses import dataclass

import credits

SOURCE = credits.SOURCE.rstrip('/')
API = 'https://api.github.com/repos/' + SOURCE.split('github.com/', 1)[1] + '/releases?per_page=30'
DOWNLOADS = SOURCE + '/releases/download/'  # the only place a zip is fetched from
HEADERS = {'User-Agent': '3DiiU-Builder', 'Accept': 'application/vnd.github+json'}
TIMEOUT, MAX_LIST, PIECE = 8, 2 << 20, 1 << 20
TOP = '3Dii U Builder'  # the zip's one top folder
EXE = TOP + '.exe'
TAG = re.compile(r'builder-([0-9]+(?:\.[0-9]+)*)(?:-[a-z]+)?-([0-9]{4})-([0-9]{2})-([0-9]{2})(?:\.([0-9]+))?')
DIGEST = re.compile(r'sha256:([0-9a-f]{64})')
ASSET = re.compile(r'3Dii[-. ]U[-. ]Builder.*\.zip', re.IGNORECASE)
RECORD, RECORD_LOCK = 'launch.json', 'launch.lock'
TRYING = 120  # seconds a handed-over version has to show its window: a fresh 620 MB app's first start under an antivirus is slow
DIED = 4  # seconds after a hand-over in which the version exiting means it failed: a window that comes up stays up far longer
LOCK_WAIT = 5  # seconds to wait for another process's turn at the record
STALE = 24 * 3600
HANDED_OVER = 'THREEDII_HANDED_OVER'


class UpdateError(Exception):
    """what went wrong, said for the person using the app"""


@dataclass
class Release:
    tag: str
    title: str
    notes: str
    url: str
    size: int
    sha256: str


def tag_key(tag):
    """builder-1.0-beta-2026-11-12(.2) as (version, date, same-day counter), which sort the way the releases came out; None for a tag that
    isn't the app's (deps-1)"""
    m = TAG.fullmatch(tag) if isinstance(tag, str) else None
    if not m:
        return None
    return tuple(int(n) for n in m[1].split('.')), (int(m[2]), int(m[3]), int(m[4])), int(m[5] or 0)


def newest(releases, skip=()):
    """The newest published app release with a zip GitHub vouches for: a digest and a download from this repository. Anything else is
    skipped, as a release can't be checked without its digest, and so is any tag in skip. None if there is none."""
    best = None
    for r in releases if isinstance(releases, list) else ():
        if not isinstance(r, dict) or r.get('draft') or r.get('prerelease') or not tag_key(r.get('tag_name')) or r.get('tag_name') in skip:
            continue
        for a in r.get('assets') or ():
            digest = DIGEST.fullmatch(str(a.get('digest') or '')) if isinstance(a, dict) else None
            url, size = digest and a.get('browser_download_url'), digest and a.get('size')
            if (digest and ASSET.fullmatch(str(a.get('name', ''))) and isinstance(url, str) and url.startswith(DOWNLOADS)
                    and isinstance(size, int) and size > 0):
                if not best or tag_key(r['tag_name']) > tag_key(best.tag):
                    best = Release(r['tag_name'], str(r.get('name') or r['tag_name']), str(r.get('body') or ''), url, size, digest[1])
                break
    return best


def check(own_tag, opener=None, skip_failed=True):
    """The release to offer, if one is newer than own_tag (an app with no tag is a development build: any release is newer). None when up to
    date, offline, or GitHub answers anything unexpected. A release that failed to start here before is passed over for the next newer one,
    unless skip_failed is False (the person asked to try it again)."""
    try:
        import urllib.request  # here, not above: app.py imports this module at every start, and ssl is slow to load
        with (opener or urllib.request.urlopen)(urllib.request.Request(API, headers=HEADERS), timeout=TIMEOUT) as r:
            data = r.read(MAX_LIST + 1)
        if len(data) > MAX_LIST:
            return None
        release = newest(json.loads(data), failed_tags() if skip_failed else ())
        mine = tag_key(own_tag)
        return release if release and (mine is None or tag_key(release.tag) > mine) else None
    except Exception:  # an update check must never be why the app fails
        return None


def clear_read_only(func, path, _):
    """rmtree's error handler: a read-only file (some zips carry the flag) is made writable and removed after all"""
    os.chmod(path, stat.S_IWRITE)
    func(path)


def remove(path):
    """deletes a file or folder if it's there, best effort"""
    try:
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path, **{'onexc' if sys.version_info >= (3, 12) else 'onerror': clear_read_only})
        elif os.path.lexists(path):
            try:
                os.remove(path)
            except PermissionError:
                os.chmod(path, stat.S_IWRITE)
                os.remove(path)
    except OSError:
        pass


def long_path(path):
    """The app's engine nests deep (devkitPPC's headers): unpacked under a folder in AppData its paths pass Windows' 260 characters, which
    only the \\\\?\\ form lets past."""
    path = os.path.abspath(path)
    return '\\\\?\\' + path if os.name == 'nt' and not path.startswith('\\\\') else path


def discard(path, tag):
    """Removes a version's folder by renaming it to trash-<tag>-<pid> first: Windows refuses that while any file in it is open, so a version
    that is running is left alone. Whether it was taken."""
    trash = os.path.join(os.path.dirname(path), f'trash-{tag}-{os.getpid()}')
    remove(long_path(trash))
    try:
        os.rename(long_path(path), long_path(trash))
    except OSError:
        return False
    remove(long_path(trash))
    return True


def need_room(folder, size, what):
    """refuses before a download or unpack fills the drive halfway"""
    free = shutil.disk_usage(folder).free
    if free < size:
        raise UpdateError(f'There isn\'t enough free space for {what}: it needs {size / 2**30:.1f} GB on that drive, and '
                          f'{free / 2**30:.1f} GB is free. Free some space and try again.')


def download(release, folder, progress=None, cancelled=None, opener=None):
    """Downloads the release's zip into folder as <tag>.zip and returns its path, once its size and SHA-256 match what GitHub published.
    None when cancelled before the last piece. A part file is never left behind, and two windows never write the same one. A whole
    <tag>.zip already there (an update whose unpacking was cut short) is used without downloading it again."""
    if not release.url.startswith(DOWNLOADS) or not tag_key(release.tag):
        raise UpdateError('That update doesn\'t come from the 3Dii U repository, so it wasn\'t downloaded.')
    path = os.path.join(folder, release.tag + '.zip')
    if whole(path, release):  # downloaded before, and its unpacking was cut short: used as it is
        if progress:
            progress(release.size, release.size)
        return path
    try:
        os.makedirs(folder, exist_ok=True)
        need_room(folder, 4 * release.size, 'the update')  # the zip, and the app unpacked from it at about three times its size
    except OSError as e:
        raise UpdateError(f'The update can\'t be saved in {folder}: {e}')
    part = os.path.join(folder, f'{release.tag}.{os.getpid()}.zip.part')
    done, sha, stopped = 0, hashlib.sha256(), False
    try:
        import urllib.request  # here, not above: app.py imports this module at every start, and ssl is slow to load
        request = urllib.request.Request(release.url, headers={'User-Agent': HEADERS['User-Agent']})
        with (opener or urllib.request.urlopen)(request, timeout=30) as r, open(part, 'wb') as f:
            while True:
                if cancelled and cancelled():
                    stopped = True
                    break
                piece = r.read(PIECE)
                if not piece:
                    break
                done += len(piece)
                if done > release.size:
                    raise UpdateError('The download is bigger than the release says, so it was thrown away. Try again later.')
                sha.update(piece)
                f.write(piece)
                if progress:
                    progress(done, release.size)
        if stopped and done < release.size:
            remove(part)
            return None
        if done != release.size or sha.hexdigest() != release.sha256:
            raise UpdateError('The download doesn\'t match the release (it was cut short or changed on the way), so it was thrown away. '
                              'Try again.')
        os.replace(part, path)
        return path
    except UpdateError:
        remove(part)
        raise
    except Exception as e:
        remove(part)
        raise UpdateError(f'The download stopped: {e}. Check the connection and try again.')


def whole(path, release):
    """whether path is already the release's zip: its size and SHA-256 are the ones GitHub published"""
    try:
        if os.path.getsize(path) != release.size:
            return False
        sha = hashlib.sha256()
        with open(path, 'rb') as f:
            for piece in iter(lambda: f.read(PIECE), b''):
                sha.update(piece)
        return sha.hexdigest() == release.sha256
    except OSError:
        return False


def verify(app):
    """The checks app.py makes at start-up, made on a freshly unpacked version before it's used: the whole app arrived, as one version"""
    try:
        with open(os.path.join(app, '_internal', 'files.txt')) as f:
            want = int(f.read())
        with open(os.path.join(app, '_internal', 'stamp.txt')) as f, open(os.path.join(app, 'engine', 'stamp.txt')) as g:
            same = f.read() == g.read()
    except (OSError, ValueError):
        raise UpdateError('The update is incomplete: files the app needs are missing from it.')
    if not os.path.isfile(os.path.join(app, EXE)):
        raise UpdateError('The update is incomplete: the app itself is missing from it.')
    if sum(len(fs) for _, _, fs in os.walk(app)) < want:
        raise UpdateError('The update is incomplete: some of its files are missing. An antivirus may have removed them.')
    if not same:
        raise UpdateError('The update holds parts of two versions of the app, so it wasn\'t used.')


def safe_name(name):
    """a zip member that can only land inside the app's own top folder"""
    parts = name.replace('\\', '/').split('/')
    return parts[0] == TOP and ':' not in name and '..' not in parts and not name.startswith(('/', '\\'))


def unpack(zip_path, folder):
    """Unpacks <tag>.zip into folder/<tag>/ and returns the app folder inside, once it passes verify() and says it is that release. It is
    unpacked beside, as <tag>.unpacking-<pid>, and renamed into place, so a failure leaves nothing behind, and a version of the same tag
    that is running is never taken from under it. A zip with any member that could land outside its folder is refused whole."""
    tag = os.path.basename(zip_path)[:-len('.zip')]
    if not zip_path.endswith('.zip') or not tag_key(tag):
        raise UpdateError('That file isn\'t a 3Dii U Builder update.')
    dest, work = os.path.join(folder, tag), os.path.join(folder, f'{tag}.unpacking-{os.getpid()}')
    remove(long_path(work))
    try:
        os.makedirs(folder, exist_ok=True)
        with zipfile.ZipFile(zip_path) as z:
            members = z.infolist()
            if not members or not all(safe_name(m.filename) for m in members):
                raise UpdateError('The update\'s zip holds files outside the app\'s folder, so it wasn\'t used.')
            need_room(folder, sum(m.file_size for m in members) + (256 << 20), 'unpacking the update')
            z.extractall(long_path(work))
        verify(long_path(os.path.join(work, TOP)))
        try:
            with open(long_path(os.path.join(work, TOP, '_internal', 'release.txt'))) as f:
                labelled = f.read(200).strip()
        except OSError:
            labelled = ''
        if labelled != tag:  # a version runs by the tag it was built with: under another one it would be offered itself as an update
            raise UpdateError('The update is labelled as another version, so it wasn\'t used.')
        if os.path.lexists(dest) and not discard(dest, tag):
            raise UpdateError('That version is in use: close the app\'s other windows and try again.')
        os.rename(long_path(work), long_path(dest))
        return os.path.join(dest, TOP)
    except UpdateError:
        remove(long_path(work))
        raise
    except Exception as e:
        remove(long_path(work))
        raise UpdateError(f'The update couldn\'t be unpacked: {e}')


def home():
    """Where downloaded versions live: in the app's data folder, beside the work folder app.py's use_engine() names, so updating never
    touches the folder the person unzipped the app into"""
    base = os.environ.get('LOCALAPPDATA') or os.path.join(os.path.expanduser('~'), '.local', 'share')
    return os.path.join(base, '3Dii U Builder', 'app')


def running_from():
    """the folder of the exe this process is"""
    return os.path.dirname(os.path.abspath(sys.executable))


def same_path(a, b):
    """whether two paths name the same folder, as Windows compares them"""
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def inside(path, folder):
    """whether path is folder or lies under it"""
    try:
        return same_path(os.path.commonpath([os.path.abspath(path), os.path.abspath(folder)]), folder)
    except ValueError:  # on another drive
        return False


def runs_from_home():
    """whether this process is a downloaded version, rather than the app the person unzipped"""
    return inside(running_from(), home())




@contextlib.contextmanager
def record_lock():
    """Yields whether this process holds launch.lock, so two starts or windows reading and rewriting the record never lose each other's
    change. Windows releases it if the process dies. Not held within LOCK_WAIT: the caller writes nothing."""
    f = None
    try:
        f = open(os.path.join(home(), RECORD_LOCK), 'a+')
        deadline = time.monotonic() + LOCK_WAIT
        while True:
            try:
                f.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() > deadline:
                    f.close()
                    f = None
                    break
                time.sleep(0.05)
    except OSError:
        f = None
    try:
        yield f is not None
    finally:
        if f:
            try:
                f.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError:
                pass
            f.close()


def read_record():
    """the launch record activate() wrote, or None"""
    try:
        with open(os.path.join(home(), RECORD)) as f:
            record = json.loads(f.read(1 << 16))
        return record if isinstance(record, dict) and record.get('format') == 1 else None
    except Exception:
        return None


def failed_tags(record=None):
    """the releases that failed to start here, from the record"""
    record = record if record is not None else read_record()
    failed = record.get('failed') if record else None
    return [t for t in failed if tag_key(t)][-20:] if isinstance(failed, list) else []


def write_record(record, state):
    """Writes the record with this state, now, keeping its list of failed releases (and adding its own tag when it failed). Replaced whole,
    so a crash leaves the old record or the new one; a temporary file of its own per process. Called with record_lock() held. Whether it
    was written."""
    failed = failed_tags(record)
    if state == 'failed' and record.get('tag') not in failed:
        failed.append(record.get('tag'))
    path = os.path.join(home(), RECORD)
    temp = f'{path}.{os.getpid()}.tmp'
    try:
        with open(temp, 'w') as f:
            json.dump({**record, 'format': 1, 'state': state, 'since': time.time(), 'failed': failed}, f)
            f.flush()
            os.fsync(f.fileno())
        for last in [False] * 19 + [True]:  # a reader (tidy, check) may have the record open for a moment: Windows refuses the replace meanwhile
            try:
                os.replace(temp, path)
                return True
            except PermissionError:
                if last:
                    raise
                time.sleep(0.05)
    except OSError:
        remove(temp)
        return False


def record_exe(record):
    """the record's exe as a path inside home(), or None for one that would lead anywhere else"""
    rel = record.get('exe')
    if not isinstance(rel, str) or not rel or os.path.isabs(rel) or os.path.splitdrive(rel)[0] or rel.startswith(('/', '\\')):
        return None
    if '..' in rel.replace('\\', '/').split('/'):
        return None
    path = os.path.abspath(os.path.join(home(), rel))
    return path if inside(path, home()) and not same_path(path, home()) else None


def activate(app):
    """Makes an unpacked, verified version (home()/<tag>/3Dii U Builder, from unpack()) the one the app hands over to from its next start,
    and removes its zip. Activating a release that failed before is a deliberate retry: it leaves the failed list."""
    app = os.path.abspath(app)
    tag = os.path.basename(os.path.dirname(app))
    if os.path.basename(app) != TOP or not tag_key(tag) or not same_path(os.path.dirname(os.path.dirname(app)), home()):
        raise UpdateError('That isn\'t a downloaded update, so it wasn\'t made the version to run.')
    verify(long_path(app))
    with record_lock() as held:
        written = held and write_record({'tag': tag, 'exe': os.path.relpath(os.path.join(app, EXE), home()),
                                         'failed': [t for t in failed_tags() if t != tag]}, 'new')
    if not written:
        raise UpdateError('The update couldn\'t be made the version to run: its folder can\'t be written to, or another window of the app is '
                          'busy with it. Try again.')
    remove(os.path.join(home(), tag + '.zip'))


def activated():
    """The tag of the version the launch record points the app at (new, trying or ok, with its exe there), or ''. Reads only; never
    fails."""
    try:
        record = read_record()
        exe = record and record.get('state') in ('new', 'trying', 'ok') and tag_key(record.get('tag')) and record_exe(record)
        return record['tag'] if exe and os.path.isfile(exe) else ''
    except Exception:
        return ''


def current(own):
    """The exe to hand over to, or None, keeping the record's state: a version is tried ('trying') and must confirm, through started(),
    within TRYING seconds that its window came up ('ok'); one that never did is set aside ('failed') at the next start, so a version that
    crashes or hangs before its window costs one start, not every one. Only a version newer than own (a development build, with no tag,
    never hands over), whose exe is there. Nothing else is checked: the version checks itself as it starts. Never hands over without
    recording it."""
    try:
        mine = tag_key(own)
        if not mine:
            return None
        with record_lock() as held:
            record = held and read_record()
            theirs = record and tag_key(record.get('tag'))
            exe = theirs and record_exe(record)
            if not theirs or theirs <= mine or not exe or not os.path.isfile(exe) or same_path(os.path.dirname(exe), running_from()):
                return None
            since, state = record.get('since'), record.get('state')
            if state == 'trying':
                if isinstance(since, (int, float)) and abs(time.time() - since) <= TRYING:
                    return exe  # another start is under way
                write_record(record, 'failed')
                return None
            if state in ('new', 'ok'):
                return exe if write_record(record, 'trying') else None
            return None
    except Exception:  # whatever is wrong with an update, the app still starts as itself
        return None


def confirm():
    """the hand-over to this downloaded version worked, and the next start makes it again. Whether it was recorded."""
    try:
        with record_lock() as held:
            record = held and read_record()
            if record and runs_from_home() and record.get('tag') == own_tag() and record.get('state') == 'trying':
                return write_record(record, 'ok')
    except Exception:
        pass
    return False


def started():
    """Called by the window once it has been shown: confirms the hand-over, then clears old versions on a thread of its own, as removing
    thousands of files under an antivirus can take tens of seconds. Never fails."""
    try:
        confirm()
        threading.Thread(target=tidy, daemon=True).start()
    except Exception:
        pass


def set_aside():
    """For a downloaded version that finds itself damaged: the next start opens the app the person unzipped, and the release isn't offered
    again unless the person asks. Whether it was set aside."""
    try:
        with record_lock() as held:
            record, own = held and read_record(), own_tag()
            if not record or (own and record.get('tag') != own):  # the record is about a newer version than this one: not this one's to judge
                return False
            return write_record(record, 'failed')
    except Exception:
        return False


held_version = None  # the lock file this process keeps open in its own version folder, for its whole life


def hold_version():
    """A downloaded version keeps a file open in its own folder while it runs: Windows then refuses to rename the folder, so discard() and
    unpack() leave it alone, whatever Windows would do with the running exe itself. Locks other processes left behind are removed: one
    still open can't be. The app the person unzipped holds nothing and writes nothing. Never fails."""
    global held_version
    try:
        folder = os.path.dirname(running_from())
        if held_version or not runs_from_home() or not tag_key(os.path.basename(folder)):
            return
        if os.name == 'nt':  # elsewhere an open file can be deleted, so another process's lock is never touched
            for name in os.listdir(folder):
                if name.startswith('in-use-') and name.endswith('.lock'):
                    remove(os.path.join(folder, name))
        held_version = open(os.path.join(folder, f'in-use-{os.getpid()}.lock'), 'a')
    except Exception:
        pass


def may_hand_over(args, environ):
    """Whether a start with these arguments (python's own flags already taken off) may hand over: only a window may. A build or a script
    the window starts must run on that window's own engine, with its output reaching the window. And one hop only: a version started by
    a hand-over never hands over again, however its paths are spelled."""
    return HANDED_OVER not in environ and not (args and (args[0] == '--pipeline' or args[0].endswith('.py')))


def tidy():
    """Removes the downloaded versions, zips and part files this install has moved past, best effort: those older than the current version
    or than this app, and whatever was left unfinished a day ago. Never the current version or this process's own, nor a version a running
    app is using (discard()). Only names an update makes are touched: anything else in home() stays."""
    try:
        record, own = read_record(), tag_key(own_tag())
        pointed = record.get('tag') if record and record.get('state') != 'failed' and tag_key(record.get('tag')) else ''
        if own and pointed and tag_key(pointed) < own:  # this app is newer than what was downloaded before it came
            pointed = ''
        keep = {pointed} | ({os.path.basename(os.path.dirname(running_from()))} if runs_from_home() else set())
        newest = max([k for k in map(tag_key, keep) if k] + ([own] if own else []), default=None)
        for name in os.listdir(home()):
            path = os.path.join(home(), name)
            try:
                stale = time.time() - os.path.getmtime(path) > STALE
                if name.startswith('trash-') or '.unpacking-' in name or (name.startswith(RECORD + '.') and name.endswith('.tmp')):
                    if stale:  # another window's unfinished work while fresh
                        remove(long_path(path))
                    continue
                base = name[:-len('.zip.part')] if name.endswith('.zip.part') else name[:-len('.zip')] if name.endswith('.zip') else name
                head, _, pid = base.rpartition('.')
                tag = head if name.endswith('.zip.part') and pid.isdigit() and tag_key(head) else base
                # a version newer than any known here may be arriving in another window of the app: it's only cleared once it has lain a day
                if tag_key(tag) and tag not in keep and (stale or (newest and tag_key(tag) < newest)):
                    if os.path.isdir(path):
                        discard(path, tag)
                    else:
                        remove(path)
            except OSError:
                continue
    except Exception:
        pass


def fresh_env():
    """The environment for starting another copy of the app: without this one's PyInstaller bootloader state, which a different copy must
    not inherit, nor the engine and work folders this one chose"""
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('_PYI_', 'THREEDII_')) and k.upper() != '_MEIPASS2'}
    env['PYINSTALLER_RESET_ENVIRONMENT'] = '1'
    return env


def hand_over(exe, argv):
    """Starts exe (from current()) with argv, in the caller's working directory, marked as handed over. True once it has stayed up DIED
    seconds, or ended cleanly before that (the person closed its window at once). A version that couldn't start, or ended at once with an
    error, has failed: the record says so, and the caller carries on as itself."""
    try:
        child = subprocess.Popen([exe] + list(argv), env={**fresh_env(), HANDED_OVER: '1'})
    except Exception:
        return False
    try:
        if child.wait(DIED) == 0:
            return True
    except subprocess.TimeoutExpired:
        return True
    try:
        with record_lock() as held:
            record = held and read_record()
            if record and record.get('state') == 'trying':  # current() set it for this exe a moment ago; anything else came after
                write_record(record, 'failed')
    except Exception:
        pass
    return False


def own_tag():
    """the release this app was built as (make_app.py writes it into _internal/release.txt), or '' for a development build"""
    if not getattr(sys, 'frozen', False):
        return ''
    try:
        with open(os.path.join(running_from(), '_internal', 'release.txt')) as f:
            return f.read().strip()
    except OSError:
        return ''
