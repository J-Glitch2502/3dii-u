"""3Dii U Builder: turns your own 3DS game dumps into games for your Wii U, installed with WUP Installer GX2.

Double-click "3Dii U Builder.pyw", or run the standalone app make_app.py builds. The window is gui.py and ui/Main.qml; the build is pipeline.py.
The standalone app is one program for all of it: run with --pipeline it's the build, and given a .py file it runs that script, the way the build
runs a few of the repository's tools with sys.executable."""
import os
import runpy
import sys

FROZEN = getattr(sys, 'frozen', False)
HERE = os.path.dirname(os.path.abspath(__file__))


def find_root():
    """the build engine: this source tree, the standalone app's own engine/ folder (make_app.py), or else the 3DS-WiiU-Recomp folder it sits in"""
    if not FROZEN:
        return os.path.dirname(HERE)
    d = os.path.dirname(os.path.abspath(sys.executable))
    if os.path.isfile(os.path.join(d, 'engine', 'tools', 'win', 'build3ds.py')):
        return os.path.join(d, 'engine')
    for _ in range(8):
        if os.path.isfile(os.path.join(d, 'tools', 'win', 'build3ds.py')):
            return d
        d = os.path.dirname(d)
    return ''


def short_path(engine, work):
    """devkitPPC's gcc can't open a header whose path passes 260 characters, and it reaches its C++ headers through bin/../lib/gcc/...
    ../../../../, some 140 characters past the app's folder: an app kept in a deep folder couldn't compile. So the build goes through a
    directory junction in the work folder, which needs no administrator and moves with the app. Without one, the app's own path."""
    if os.name != 'nt' or 'THREEDII_ROOT' in os.environ:  # a build the app started inherits the window's choice
        return engine
    link = os.path.join(work, 'engine')
    try:
        if os.path.lexists(link):
            if os.path.isdir(link) and os.path.normcase(os.path.realpath(link)) == os.path.normcase(os.path.realpath(engine)):
                return link
            os.rmdir(link)  # a junction to where the app was before, or to nothing now: rmdir takes the link, never what it pointed to
        os.makedirs(work, exist_ok=True)
        import subprocess
        made = subprocess.run(['cmd', '/c', 'mklink', '/J', link, engine], capture_output=True, creationflags=0x08000000).returncode == 0
        return link if made else engine
    except OSError:
        return engine


def fresh_cache(engine, work):
    """The work folder keeps the compiled runtime for the next game, and build3ds recompiles a file only when its source is newer than its
    object. A newer app's sources can be older than objects an earlier app compiled there, so with another engine the cache starts over."""
    mark = os.path.join(work, 'engine_stamp.txt')
    try:
        with open(os.path.join(engine, 'stamp.txt')) as f:  # make_app.py writes one into each engine
            stamp = f.read()
        if os.path.isfile(mark):
            with open(mark) as f:
                if f.read() == stamp:
                    return
        import shutil
        shutil.rmtree(os.path.join(work, 'build_win_builder'), ignore_errors=True)
        os.makedirs(work, exist_ok=True)
        with open(mark, 'w') as f:
            f.write(stamp)
    except OSError:  # an engine with no stamp, or no work folder: builds go on with the cache as it is
        pass


def use_engine(root):
    """With its own engine the app brings its toolchain too, and builds in a folder of the user's: its own folder may be read-only."""
    if root and os.path.basename(root) == 'engine':
        base = os.environ.get('LOCALAPPDATA') or os.path.join(os.path.expanduser('~'), '.local', 'share')
        work = os.environ.setdefault('THREEDII_WORK', os.path.join(base, '3Dii U Builder'))
        if 'THREEDII_ROOT' not in os.environ:  # once, in the window: the builds it starts inherit the cache as it left it
            fresh_cache(root, work)
        root = short_path(root, work)
        os.environ.setdefault('THREEDII_DEVKITPRO', os.path.join(root, 'devkitpro').replace('\\', '/'))
        os.environ.setdefault('THREEDII_SHADERGEN', os.path.join(root, 'bin', 'shadergen.exe'))
    os.environ.setdefault('THREEDII_ROOT', root)  # pipeline.py reads these, and the build's own processes inherit them


PYTHON_FLAGS = ('-B', '-E', '-I', '-O', '-OO', '-P', '-S', '-s', '-u')  # python.exe's own options, which the build's scripts pass along


def hand_over():
    """An app with a downloaded update opens that version's window instead (updater.py), so the folder the person unzipped is never changed.
    Holding Shift while it starts opens this one. Done before use_engine(), which would otherwise start the work folder's compiled cache
    over for this engine at every start, and the newer version's again after it."""
    if not FROZEN:
        return
    try:
        import ctypes
        import updater
        args = sys.argv[1:]
        while args and args[0] in PYTHON_FLAGS:
            args = args[1:]
        if updater.may_hand_over(args, os.environ) and not ctypes.windll.user32.GetAsyncKeyState(0x10) & 0x8000:
            exe = updater.current(updater.own_tag())
            if exe and updater.hand_over(exe, sys.argv[1:]):
                sys.exit(0)
        updater.hold_version()  # a downloaded version, in any mode: its folder mustn't be cleared away while it runs
    except Exception:  # an update must never stop the app starting as itself
        pass


hand_over()
use_engine(find_root())


def quiet_children():
    """The standalone app is a windowed program with no console to hand down, so every compiler the build starts would open a console
    window of its own: they're started without one."""
    if os.name != 'nt' or not FROZEN:
        return
    import subprocess
    start = subprocess.Popen.__init__

    def init(self, *args, creationflags=0, **kw):
        start(self, *args, creationflags=creationflags | 0x08000000, **kw)  # CREATE_NO_WINDOW
    subprocess.Popen.__init__ = init


def run_script(argv):
    """python.exe's part for the standalone app: the script runs as __main__ with its own folder first on the path"""
    sys.argv = list(argv)
    sys.path.insert(0, os.path.dirname(os.path.abspath(argv[0])))
    runpy.run_path(argv[0], run_name='__main__')


def headless(work):
    """The build and the scripts write to the window's log, never to the screen: an error is printed and ends the process. Left alone, the
    standalone app would show it in a box of its own and wait there for someone to close it, with the build hanging behind it."""
    try:
        quiet_children()
        # The window reads this as UTF-8. Left to Windows, Python writes the PC's own code page, and the first character that lacks (the ★
        # in Kumamon's name, the ™ in Street Fighter's: two testers, 2026-11-11) ends the build with an encoding error.
        for s in (sys.stdout, sys.stderr):
            if s:
                s.reconfigure(encoding='utf-8', errors='backslashreplace')
        work()
    except SystemExit:
        raise
    except BaseException:
        import traceback
        traceback.print_exc()
        sys.stdout and sys.stdout.flush()
        sys.exit(1)


def unpacked_whole():
    """A zip unpacked only in part (Windows' Extract All stops at a path it finds too long, an antivirus takes a file) makes an app that fails
    in ways no tester can read. make_app.py writes down how many files the app has; fewer here means some never arrived."""
    if not FROZEN:
        return True
    app = os.path.dirname(os.path.abspath(sys.executable))
    try:
        with open(os.path.join(app, '_internal', 'files.txt')) as f:
            want = int(f.read())
    except (OSError, ValueError):
        return False
    return sum(len(fs) for _, _, fs in os.walk(app)) >= want  # more is fine: the build's scripts may leave caches


def one_version():
    """A new app unzipped over an older one, or its .exe copied into the old folder, runs the new build steps on the old engine, and fails in
    ways no tester can read (a tester's Bloo Kid 2, 2026-11-11). make_app.py gives the app and its engine the same stamp."""
    app = os.path.dirname(os.path.abspath(sys.executable))
    if not FROZEN or not os.path.isdir(os.path.join(app, 'engine')):
        return True
    try:
        with open(os.path.join(app, '_internal', 'stamp.txt')) as f, open(os.path.join(app, 'engine', 'stamp.txt')) as g:
            return f.read() == g.read()
    except OSError:
        return False


MISSING = ('Some of its files are missing.\n\nUnzip the whole zip again, into a short folder such as C:\\Games: Windows can\'t unzip into deep '
           'folders and skips files there. If it happens again, check whether your antivirus removed any of them.')
MIXED = ('This folder holds parts of two versions of the app.\n\nDelete the folder, then unzip the whole new zip into an empty one: don\'t unzip '
         'a new version over an old one. Your games and settings are kept elsewhere and stay.')
DAMAGED = ('The downloaded update is damaged, so it was set aside.\n\nStart 3Dii U Builder again: the version you had opens, and can download '
           'the update afresh.')


def cannot_start(why=MISSING):
    """said before Qt loads, as Qt may be what's missing. A downloaded update that is damaged is set aside, so the app the person unzipped
    opens next time instead of handing over to it again."""
    try:
        import updater
        if FROZEN and updater.runs_from_home() and updater.set_aside():
            why = DAMAGED
    except Exception:  # the damage may include updater.py itself
        pass
    import ctypes
    ctypes.windll.user32.MessageBoxW(None, why, '3Dii U Builder can\'t start', 0x10)
    sys.exit(1)


def main():
    while len(sys.argv) > 1 and sys.argv[1] in PYTHON_FLAGS:
        del sys.argv[1]
    if len(sys.argv) > 1 and sys.argv[1] == '--pipeline':
        if sys.stdout:
            sys.stdout.reconfigure(line_buffering=True)
        sys.argv = [os.path.join(HERE, 'pipeline.py')] + sys.argv[2:]

        def build():
            import pipeline
            pipeline.main()
        headless(build)
    elif len(sys.argv) > 1 and sys.argv[1].endswith('.py'):
        argv = sys.argv[1:]
        headless(lambda: run_script(argv))
    else:
        if not unpacked_whole():
            cannot_start()
        if not one_version():
            cannot_start(MIXED)
        try:
            import gui
        except ImportError as e:  # a file the count can't miss: one an antivirus emptied, say
            if not FROZEN:
                raise
            cannot_start(f'{MISSING}\n\n({e})')
        sys.exit(gui.run())


if __name__ == '__main__':
    main()
