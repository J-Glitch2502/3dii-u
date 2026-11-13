"""Self-check for the window (ui/Main.qml), unseen: it loads without a QML warning, every app.<name> it uses exists on App (a missing one is a
dead button), each page, build view, list and dialog loads cleanly with its delegates made, every pill has a name for screen readers, the
theme switches, and the updater's part walks its states with updater's network and disk calls faked. python builder/test_window.py"""
import json
import os
import re
import shutil
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ['QT_QPA_PLATFORM'] = 'offscreen'
if sys.platform == 'win32':
    os.environ['QT_QPA_FONTDIR'] = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')  # Qt ships no fonts for the offscreen platform
sys.path.insert(0, HERE)

from PySide6.QtCore import QCoreApplication, QMetaObject, QObject, QUrl, qInstallMessageHandler  # noqa: E402
from PySide6.QtGui import QColor, QImage  # noqa: E402
from PySide6.QtQuick import QQuickWindow  # noqa: E402
import shiboken6  # noqa: E402

warnings = []
qInstallMessageHandler(lambda kind, ctx, msg: warnings.append(msg) if 'qml' in msg.lower() or 'Main.qml' in msg else None)
import gui  # noqa: E402

base = tempfile.mkdtemp()
settings = os.path.join(base, 'settings.json')
gui.settings_file = lambda: settings  # the user's settings are never read or written, on any platform
gui.updater.home = lambda: os.path.join(base, 'app')  # nor the downloaded versions' folder
os.makedirs(os.path.join(base, 'keys'))
json.dump({'keys': os.path.join(base, 'keys')}, open(settings, 'w'))  # an empty key folder: the user's keys are never looked at
try:
    qt, engine, state = gui.create(['test'])
    engine.warnings.connect(lambda ws: warnings.extend(w.toString() for w in ws))
    assert engine.rootObjects(), warnings
    win = shiboken6.wrapInstance(shiboken6.getCppPointer(engine.rootObjects()[0])[0], QQuickWindow)
    assert not win.isVisible(), 'the window shows itself on load: gui.run() is what shows it'

    def settle(what):
        for _ in range(5):
            QCoreApplication.processEvents()
        assert not warnings, f'{what}: {warnings}'

    def items(item):
        yield item
        for child in item.childItems():
            yield from items(child)

    def named_pills(what):
        pills = [p for p in items(QQuickWindow.contentItem(win)) if p.objectName() == 'pill' and p.isVisible()]
        unnamed = [p.property('text') for p in pills if not p.property('accessibleName')]
        assert not unnamed, f'{what}: pills without an accessible name: {unnamed}'
        return len(pills)

    settle('loading')
    used = set(re.findall(r'\bapp\.(\w+)', open(os.path.join(HERE, 'ui', 'Main.qml'), encoding='utf-8').read()))
    missing = sorted(n for n in used if not hasattr(state, n))
    assert not missing, f'Main.qml uses app.{missing}, which App lacks'

    art = os.path.join(base, 'art.png')
    img = QImage(64, 64, QImage.Format_RGB32)
    img.fill(QColor('teal'))
    img.save(art)
    icon = gui.icon_url([[(90, 120, 150)] * 48] * 48)
    state.set(hasGame=True, gameName='A game', badge='Untested', icon=icon, hasArt=True, artUrl=QUrl.fromLocalFile(art).toString(),
              library=[dict(path='C:/a.3ds', name='One', icon=icon, cover='', current=True), dict(path='C:/b.3ds', name='Two', icon=icon, cover='', current=False)],
              cards=[dict(path='E:\\', label='E:\\    set up for a Wii U'), dict(path='F:\\', label='F:\\    removable drive')],
              games=[dict(name='Finished', status='Complete', note='A note.')] + state.games)
    for page in ('games', 'help', 'build'):
        win.setProperty('page', page)
        settle(f'page {page}')
        assert named_pills(f'page {page}')
    win.setProperty('openFaq', 0)
    settle('an open answer')
    menu = win.findChild(QObject, 'cardMenu')
    menu.setProperty('visible', True)
    settle('the card menu')
    assert not win.findChild(QObject, 'pages').property('enabled'), 'Tab reaches the pages behind the card menu'
    menu.setProperty('visible', False)
    stages = ['Reading', 'Recompiling', 'Packing']
    for view in ('building', 'done', 'failed', 'form'):
        state.set(view=view, stages=stages, stage=1, failed=view == 'failed', building=view == 'building', resultText='x', log='a\nb')
        win.setProperty('showLog', view == 'failed')
        settle(f'view {view}')
    for yes in ('Yes', 'Sure'):  # two questions queued: the second starts on its own yes button too
        state.dialogs.append(dict(title='A question with a title long enough to wrap onto a second line', message='A message', yes=yes, no='No',
                                  tone='ask', flow=False))
    state.show_dialog()
    settle('a question')
    assert win.findChild(QObject, 'dialog').property('visible'), 'the dialog did not show'
    named_pills('a question')

    def focused_pill():
        item = win.activeFocusItem()
        return item.parentItem().property('text') if item and item.parentItem() else None

    assert focused_pill() == 'Yes', focused_pill()
    no = next(p for p in items(QQuickWindow.contentItem(win)) if p.objectName() == 'pill' and p.isVisible() and p.property('text') == 'No')
    QMetaObject.invokeMethod(no, 'focusIn')  # answered with "No" focused, as a keyboard user would
    assert focused_pill() == 'No', focused_pill()
    state.answer(False)
    settle('the next question')
    assert focused_pill() == 'Sure', f'the next question starts on {focused_pill()!r}, not its yes button'
    state.answer(False)
    state.tell('A title', 'A message')
    settle('a message')
    state.answer(True)
    assert state.dialog is None
    assert state.faq and all(q.get('q') and q.get('a') for q in state.faq), 'faq.json is empty'
    # a backslash in a path written through a Python string can become a control character: none may reach a reader
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app_readme.txt'), encoding='utf-8') as f:
        words = f.read() + ''.join(q['q'] + q['a'] for q in state.faq) + ''.join(''.join(h) for h in gui.HELP.values())
    assert not [c for c in words if ord(c) < 32 and c not in '\r\n\t'], 'a control character in the read-me, the questions or the help texts'
    state.setThemeMode('dark')
    assert state.dark and state.themeMode == 'dark'
    settle('dark')
    state.setThemeMode('light')
    assert not state.dark

    # updates, with updater's network and disk calls faked: each worker is waited on, and its result delivered through the signal
    up = gui.updater
    TAG = 'builder-9.9-2030-01-01'
    release = up.Release(TAG, 'Builder 9.9', 'What changed. ' * 50, up.DOWNLOADS + 'x.zip', 10, 'a' * 64)
    calls = {'check': [], 'started': 0, 'activate': [], 'quit': 0, 'download': 0, 'stopped': 0}
    fake = {'release': release, 'activated': '', 'failed': [], 'gate': threading.Event()}
    fake['gate'].set()
    seen = []
    state.changed.connect(lambda: seen.append((state.updateState, state.updateProgress)))

    def done(what):
        for _ in range(3):  # a result can start another worker (an asked check after the automatic one)
            for job in (state.check_job, state.update_job):
                if job:
                    job.join(10)
                    assert not job.is_alive(), f'{what}: a worker is still running'
            settle(what)

    def check(own, opener=None, skip_failed=True):
        calls['check'].append(skip_failed)
        if skip_failed:  # the automatic check waits for the gate, so a test can act while it runs
            fake['gate'].wait(10)
        return fake['release']

    def download(rel, folder, progress=None, cancelled=None, opener=None):
        calls['download'] += 1
        progress(5, 10)
        progress(10, 10)
        return os.path.join(folder, rel.tag + '.zip')

    def stopped_download(rel, folder, progress=None, cancelled=None, opener=None):
        calls['download'] += 1
        while not cancelled():  # until cancelUpdate(); the join's timeout catches a cancel that never arrives
            threading.Event().wait(0.01)
        return None

    def broken_download(*a, **kw):
        raise up.UpdateError('There isn\'t enough free space for the update.')

    def activate(app):
        calls['activate'].append(app)
        fake['activated'] = TAG

    def started():
        calls['started'] += 1

    def pill(text):
        return next(p for p in items(QQuickWindow.contentItem(win)) if p.objectName() == 'pill' and p.isVisible() and p.property('text') == text)

    up.check, up.download, up.unpack, up.activate, up.started = check, download, lambda z, f: os.path.join(f, 'app'), activate, started
    up.activated, up.failed_tags = lambda: fake['activated'], lambda: fake['failed']
    gui.QApplication = type('NoQuit', (), {'quit': staticmethod(lambda: calls.__setitem__('quit', calls['quit'] + 1))})
    state.stop_for_close = lambda: calls.__setitem__('stopped', calls['stopped'] + 1)

    # a version made ready before this start shows as ready at once; only failures newer than this app are noted
    real_own = up.own_tag
    up.own_tag = lambda: 'builder-1.0-2026-11-12'
    fake['activated'] = TAG
    assert gui.App().updateState == 'ready', 'a version downloaded before is not shown as ready'
    fake['activated'] = 'builder-0.9-2026-01-01'
    assert gui.App().updateState == '', 'an older downloaded version is offered'
    fake['activated'], fake['failed'] = '', ['builder-0.9-2026-01-01']
    assert not gui.App().updateSetAside, 'a failure older than this app is noted'
    fake['failed'] = [TAG]
    assert gui.App().updateSetAside
    up.own_tag, fake['failed'] = real_own, []

    for _ in range(2):  # the first frame, and a second: started() runs once
        win.frameSwapped.emit()
    settle('first frames')
    for t in [t for t in threading.enumerate() if t is not threading.main_thread()]:
        t.join(10)
    assert calls['started'] == 1, calls['started']
    assert not state.auto_check.isActive() and not calls['check'], 'a development build checked for updates by itself'

    fake['release'] = None
    state.checkUpdate()
    done('a check with nothing new')
    assert state.updateState == 'none' and calls['check'] == [False], 'Check for updates must offer a release that failed before'
    fake['release'] = release

    fake['gate'].clear()
    state.check_update(False)  # the automatic check, held at the gate
    state.checkUpdate()  # pressed meanwhile: an asked check follows it
    assert state.updateState == 'checking'
    fake['gate'].set()
    done('a check asked during the automatic one')
    assert calls['check'][-2:] == [True, False] and state.updateState == 'available', (calls['check'], state.updateState)
    assert state.updateTitle == 'Builder 9.9' and len(state.updateNotes) == 400
    win.setProperty('page', 'build')
    settle('the offer')
    pill('Update')
    win.setProperty('page', 'help')
    settle('the offer on the Help page')
    win.setProperty('page', 'build')

    seen.clear()
    state.startUpdate()
    state.startUpdate()  # a second press while one runs does nothing
    done('an update')
    states = [s for s, _ in seen]
    assert states[0] == 'downloading' and 'unpacking' in states and states[-1] == 'ready', states
    assert ('downloading', 50) in seen and calls['activate'] == [os.path.join(up.home(), 'app')] and calls['download'] == 1, (seen, calls)
    state.startUpdate()
    assert state.updateState == 'ready' and calls['download'] == 1, 'Update acts only on an offer or a failure'

    state.laterUpdate()
    state.checkUpdate()
    done('a check after Later')
    assert state.updateState == 'ready' and calls['download'] == 1, 'what is already installed is offered again'

    fake['activated'] = ''
    state.set(updateState='available')
    up.download = broken_download
    state.startUpdate()
    done('a failed download')
    assert state.updateState == 'failed' and 'free space' in state.updateMessage

    up.download = stopped_download
    fake['gate'].clear()
    state.set(updateState='')
    state.check_update(False)  # held at the gate while the download starts
    state.set(updateState='available')
    state.startUpdate()
    fake['gate'].set()
    state.check_job.join(10)
    settle('a check ending during a download')
    assert state.updateState == 'downloading', 'a check took the card away from the download'
    state.cancelUpdate()
    done('a cancelled download')
    assert state.updateState == 'available', state.updateState

    up.download = download
    state.restartForUpdate()
    assert state.updateState == 'available', 'a restart before the update is ready'
    state.set(updateState='ready', building=True)
    up.current = lambda own: calls.__setitem__('current', True)
    state.restartForUpdate()
    assert state.dialog and 'build' in state.dialog['title'].lower() and 'current' not in calls, 'a restart during a build must be refused'
    state.answer(True)
    state.set(building=False)

    held = threading.Event()
    up.current, up.hand_over = lambda own: 'new.exe', lambda exe, argv: held.wait(10)
    state.restartForUpdate()
    settle('restarting')
    assert state.updateState == 'restarting' and not pill('Build It!').property('enabled'), 'a build can start during the restart'
    held.set()
    done('a restart')
    assert calls['quit'] == 1 and calls['stopped'] == 1, 'the app must cancel a build and quit once the new version is up'

    state.set(updateState='ready')
    up.hand_over = lambda exe, argv: False
    state.restartForUpdate()
    done('a restart that failed')
    assert state.updateState == 'failed' and 'keeps running' in state.updateMessage and calls['quit'] == 1
    before = calls['download']
    state.startUpdate()  # Try again: the whole update again, which clears its failed mark
    done('trying again')
    assert state.updateState == 'ready' and calls['download'] == before + 1, (state.updateState, calls)

    state.set(updateState='ready')
    up.current = lambda own: None
    state.restartForUpdate()
    done('a restart with nothing to start')
    assert state.updateState == 'failed' and calls['quit'] == 1
    fake['activated'] = ''  # the failed restart set it aside
    state.checkUpdate()
    done('a check after a failed restart')
    assert state.updateState == 'available', state.updateState
    state.startUpdate()
    done('an update after a failed restart')
    assert calls['download'] == before + 2 and state.updateState == 'ready'

    for st in ('downloading', 'unpacking', 'ready', 'restarting', 'failed'):
        state.set(updateState=st, updateProgress=37)
        settle(f'the update card, {st}')
    del engine  # before the App it shows, or its bindings read a gone app on the way out
    print('window ok')
finally:
    shutil.rmtree(base, ignore_errors=True)
