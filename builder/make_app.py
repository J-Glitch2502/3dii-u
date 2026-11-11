"""Builds the standalone 3Dii U Builder with PyInstaller: builder/dist/3Dii U Builder/3Dii U Builder.exe, which runs anywhere, with no Python
and no toolchain installed.

The app is one program (app.py): the window, the build (--pipeline), and python.exe's part for the repository's tool scripts the build runs.
Its engine/ folder holds what a build needs, in the repository's own layout: the recompiler, runtime and port sources, the build scripts,
devkitPro, the shader compilers, and what the games' runs learned: where their code starts and the render states they met, with the vertex
programs they ran named by fingerprints (shaderlogs.py), as those are the games' own code. None of the games' code or data goes in.
The 3DS system archives aren't in it: they are each user's own files, imported from their emulator into the work folder.
Run: python builder/make_app.py (needs the toolchain installed here, as the shader generator is built for the app)"""
import glob
import os
import shutil
import sys
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # python -P leaves the script's folder off the path
import shaderlogs  # noqa: E402

ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, 'dist')
DEVKITPRO = 'C:/devkitPro/opt/devkitpro'
ENGINE_DIRS = ('tools', 'phase0/tools', 'phase0/title_code', 'port','third_party/3dsrecomp/runtime', 'third_party/3dsrecomp/tools', 'third_party/3dsrecomp/games',
               'third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc', 'toolchain/aac_ppc')
ENGINE_FILES = ('third_party/cafeglsl/glslcompiler.exe', 'third_party/cafeglsl/libwinpthread-1.dll', 'third_party/cafeglsl/zlib1.dll',
                'third_party/3dsrecomp/LICENSE')
WORK = os.path.join(HERE, 'build_app')  # PyInstaller's own work folder, gone afterwards
NAME = '3Dii U Builder'
# what the build's scripts import that the window's code doesn't (they're run from the repository by path, so PyInstaller can't see them)
SCRIPT_MODULES = ('urllib.request', 'ssl',  # covers.py, when the window is the frozen app
                  'argparse', 'bisect', 'builtins', 'concurrent.futures', 'filecmp', 'glob', 'hashlib', 'mmap', 'runpy', 'shlex', 'zlib',
                  'cryptography.hazmat.primitives.ciphers')  # the last for phase0/tools/pack_wup.py, which packs the WUP
EXCLUDE = ('numpy', 'PIL', 'tkinter', 'PySide6.QtWebEngineCore', 'PySide6.QtWebEngineWidgets', 'PySide6.QtWebEngineQuick', 'PySide6.QtPdf',
           'PySide6.QtMultimedia', 'PySide6.Qt3DCore', 'PySide6.QtCharts', 'PySide6.QtDataVisualization', 'PySide6.QtGraphs', 'PySide6.QtQuick3D')
# PyInstaller takes every Qt Quick module along; the window imports only QtQuick's own (Controls, Layouts, Shapes, Effects, Particles)
KEEP_QML = ('QtQuick', 'QtQml')
DROP_QT = ('Qt6WebEngine', 'Qt6Pdf', 'Qt6Quick3D', 'Qt63D', 'Qt6Charts', 'Qt6DataVisualization', 'Qt6Graphs', 'Qt6Location', 'Qt6Multimedia',
           'Qt6Positioning', 'Qt6RemoteObjects', 'Qt6Scxml', 'Qt6StateMachine', 'Qt6Sensors', 'Qt6Test', 'Qt6TextToSpeech', 'Qt6WebChannel',
           'Qt6WebSockets', 'Qt6WebView', 'Qt6SpatialAudio', 'Qt6VirtualKeyboard', 'Qt6Bluetooth', 'Qt6Nfc', 'Qt6SerialPort', 'QtWebEngineProcess',
           'opengl32sw')


def prune(qt):
    """what the window never loads, out of the app's PySide6 folder"""
    freed = 0
    for d in os.listdir(os.path.join(qt, 'qml')):
        if d not in KEEP_QML:
            p = os.path.join(qt, 'qml', d)
            freed += sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(p) for f in fs)
            shutil.rmtree(p)
    for name in os.listdir(qt):
        p = os.path.join(qt, name)
        if os.path.isfile(p) and name.startswith(DROP_QT):
            freed += os.path.getsize(p)
            os.remove(p)
    # ui/Main.qml uses the Basic style alone, and FluentWinUI3's images have the app's deepest paths, past what Extract All unzips in deep folders
    for d in ('translations', 'resources', *(os.path.join('qml', 'QtQuick', 'Controls', s) for s in
                                             ('FluentWinUI3', 'Fusion', 'Imagine', 'Material', 'Native', 'Universal', 'Windows', 'designer'))):
        p = os.path.join(qt, d)
        if os.path.isdir(p):
            freed += sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(p) for f in fs)
            shutil.rmtree(p)
    return freed


def shadergen():
    """the shader generator, built with this PC's host compiler (LLVM and MinGW) through build3ds, as the app has none"""
    import importlib.util
    spec = importlib.util.spec_from_file_location('build3ds', os.path.join(ROOT, 'tools', 'win', 'build3ds.py'))
    b3 = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(b3)
    b3.WORK = os.path.join(ROOT, 'build_win_builder').replace('\\', '/')
    return b3.build_shadergen(types.SimpleNamespace(jobs=4, force=False, extra_flags=None, only=None, opt=None))


def bundle_logs(src, dst):
    """A game's shader logs as the app ships them (shaderlogs.py): fragment records as they are, each vertex record as a fingerprint or a recipe,
    as its program words are the game's code. dvlb.bin is that code too, and the other_ files are other games' copies: the build makes both again
    from the dump. Returns (records as recipes, records left out)."""
    logs, out = os.path.join(src, 'shader_logs'), os.path.join(dst, 'shader_logs')
    os.makedirs(out, exist_ok=True)
    dvlb = os.path.join(logs, 'dvlb.bin')
    progs = shaderlogs.programs(dvlb) if os.path.isfile(dvlb) else set()
    recipes, shipped = 0, set()
    for f in [f for f in os.listdir(logs) if f.endswith('.vsr')]:  # made from a dump this repository doesn't keep (shaderlogs.py --recipes)
        shutil.copy2(os.path.join(logs, f), os.path.join(out, f))
        with open(os.path.join(logs, f), 'rb') as h:
            made = list(shaderlogs.recipes(h.read()))
        shipped |= {fp for _, fp, *_ in made}
        recipes += len(made)
    srcs, cache, left = shaderlogs.sources(src), {}, 0  # a tested game's own code.bin and romfs.bin
    for f in os.listdir(logs):
        if not f.endswith('.bin') or f == 'dvlb.bin' or f.startswith('other_'):
            continue
        with open(os.path.join(logs, f), 'rb') as h:
            frag, vsf, vsr, missing = shaderlogs.split(h.read(), progs, srcs, cache)
        recipes += sum(1 for _ in shaderlogs.recipes(vsr))
        left += sum(1 for code, swz in missing if shaderlogs.fingerprint(code, swz) not in shipped)
        for name, data in ((f, frag), (f[:-4] + '.vsf', vsf), (f[:-4] + '.vsr', vsr)):
            if data:
                with open(os.path.join(out, name), 'wb') as h:
                    h.write(data)
    shaderlogs.close(srcs)
    return recipes, left


def bundle_engine(engine):
    """the build engine, in the repository's layout, so the app builds wherever it is"""
    def skip(folder, names):  # caches, links WSL left that Windows can't open (third_party/3dsrecomp/games/*/profile.txt), and the tools
        # for testing on this PC's own console (its address, this PC's paths), which no build runs
        console = os.path.normcase(folder) == os.path.normcase(os.path.join(ROOT, 'tools'))
        return [n for n in names if n == '__pycache__' or n.endswith('.pyc') or (console and n.startswith('console_'))
                or os.lstat(os.path.join(folder, n)).st_file_attributes & 0x400]
    for d in ENGINE_DIRS:
        shutil.copytree(os.path.join(ROOT, d), os.path.join(engine, d), ignore=skip)
    for f in ENGINE_FILES:
        os.makedirs(os.path.dirname(os.path.join(engine, f)), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, f), os.path.join(engine, f))
    recipes = left = 0
    for g in sorted(os.listdir(os.path.join(ROOT, 'games'))):  # what runs learned: where code starts, the render states they met
        src, dst = os.path.join(ROOT, 'games', g), os.path.join(engine, 'games', g)
        if os.path.isfile(os.path.join(src, 'functions.txt')):
            os.makedirs(dst, exist_ok=True)
            shutil.copy2(os.path.join(src, 'functions.txt'), dst)
        if os.path.isdir(os.path.join(src, 'shader_logs')):
            made, missing = bundle_logs(src, dst)
            recipes, left = recipes + made, left + missing
    print(f'shader logs: {recipes} vertex records as recipes, {left} left out, their programs in no file of the dump to find them by')
    # libstdc++'s policy-based data structures: nothing here uses them, and theirs are the deepest paths, past what Windows can copy in a deep folder
    shutil.copytree(DEVKITPRO, os.path.join(engine, 'devkitpro'), ignore=shutil.ignore_patterns('pb_ds'))
    os.makedirs(os.path.join(engine, 'bin'))
    shutil.copy2(shadergen(), os.path.join(engine, 'bin', 'shadergen.exe'))
    with open(os.path.join(engine, 'stamp.txt'), 'w') as f:  # which engine this is: app.py starts the compiled cache over for another one
        f.write(str(time.time_ns()))
    return sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(engine) for f in fs)


def make_icon(path):
    """the window's icon as a Windows .ico, at the sizes Explorer and the taskbar use"""
    os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
    from PySide6.QtGui import QGuiApplication
    from PIL import Image
    QGuiApplication(['make_app'])
    import gui
    png = path + '.png'
    gui.draw_icon(256).save(png)
    Image.open(png).save(path, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    os.remove(png)


def main():
    import PyInstaller.__main__
    os.makedirs(WORK, exist_ok=True)
    ico = os.path.join(WORK, 'icon.ico')
    make_icon(ico)
    args = [os.path.join(HERE, 'app.py'), '--name', NAME, '--windowed', '--onedir', '--noconfirm', '--clean', '--distpath', OUT,
            '--workpath', WORK, '--specpath', WORK, '--icon', ico, '--paths', HERE,
            '--add-data', f'{os.path.join(HERE, "ui")}{os.pathsep}ui',
            '--add-data', f'{os.path.join(HERE, "compat.json")}{os.pathsep}.',
            '--add-data', f'{os.path.join(HERE, "icon.png")}{os.pathsep}.',
            '--add-data', f'{os.path.join(HERE, "bin", "makerom.exe")}{os.pathsep}bin',
            '--collect-submodules', 'pyctr', '--hidden-import', 'gui', '--hidden-import', 'covers', '--hidden-import', 'cia_to_decrypted_3ds']
    for m in SCRIPT_MODULES:
        args += ['--hidden-import', m]
    for m in EXCLUDE:
        args += ['--exclude-module', m]
    PyInstaller.__main__.run(args)
    shutil.rmtree(WORK, ignore_errors=True)
    freed = prune(os.path.join(OUT, NAME, '_internal', 'PySide6'))
    engine = bundle_engine(os.path.join(OUT, NAME, 'engine'))
    print(f'{NAME}: {os.path.join(OUT, NAME, NAME + ".exe")} ({freed >> 20} MB of unused Qt left out, engine {engine >> 20} MB)')
    import gui
    app = os.path.join(OUT, NAME)
    with open(os.path.join(HERE, 'app_readme.txt'), encoding='utf-8') as f:
        readme = f.read().replace('{version}', gui.VERSION)
    with open(os.path.join(app, 'Read me first.txt'), 'w', encoding='utf-8') as f:
        f.write(readme)
    shutil.copyfile(os.path.join(HERE, 'app_notices.txt'), os.path.join(app, 'Third-party notices.txt'))
    files = sum(len(fs) for _, _, fs in os.walk(app)) + 1  # and files.txt, which app.py checks by that a tester unzipped the whole app
    with open(os.path.join(app, '_internal', 'files.txt'), 'w') as f:
        f.write(str(files))
    for old in glob.glob(os.path.join(OUT, '*.zip')):  # one zip, this build's
        os.remove(old)
    z = shutil.make_archive(os.path.join(OUT, f'{NAME} {gui.VERSION} beta'), 'zip', OUT, NAME)
    print(f'for testers: {z} ({os.path.getsize(z) >> 20} MB)')


if __name__ == '__main__':
    main()
