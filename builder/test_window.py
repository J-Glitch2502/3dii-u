"""Self-check for the window (ui/Main.qml), unseen: it loads without a QML warning, every app.<name> it uses exists on App (a missing one is a
dead button), each page, build view, list and dialog loads cleanly with its delegates made, every pill has a name for screen readers, and the
theme switches. python builder/test_window.py"""
import json
import os
import re
import shutil
import sys
import tempfile

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
    state.setThemeMode('dark')
    assert state.dark and state.themeMode == 'dark'
    settle('dark')
    state.setThemeMode('light')
    assert not state.dark
    del engine  # before the App it shows, or its bindings read a gone app on the way out
    print('window ok')
finally:
    shutil.rmtree(base, ignore_errors=True)
