"""Self-check for the window (ui/Main.qml), unseen: it loads without a QML warning, every app.<name> it uses exists on App (a missing one is a
dead button), each page, build view, list and dialog loads cleanly with its delegates made, every pill has a name for screen readers, the
theme switches, and the updater's part walks its states with updater's network and disk calls faked; and the simple window (ui/Simple.qml)
loads as cleanly and shows the app's questions. python builder/test_window.py"""
import json
import os
import re
import shutil
import sys
import tempfile
import threading
from types import SimpleNamespace
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ['QT_QPA_PLATFORM'] = 'offscreen'
if sys.platform == 'win32':
    os.environ['QT_QPA_FONTDIR'] = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')  # Qt ships no fonts for the offscreen platform
sys.path.insert(0, HERE)

from PySide6.QtCore import Q_ARG, QCoreApplication, QMetaObject, QObject, QUrl, qInstallMessageHandler  # noqa: E402
from PySide6.QtGui import QColor, QImage  # noqa: E402
from PySide6.QtQuick import QQuickWindow  # noqa: E402
import shiboken6  # noqa: E402

warnings = []
qInstallMessageHandler(lambda kind, ctx, msg: warnings.append(msg) if 'qml' in msg.lower() else None)
import gui  # noqa: E402

base = tempfile.mkdtemp()
settings = os.path.join(base, 'settings.json')
gui.settings_file = lambda: settings  # the user's settings are never read or written, on any platform
gui.updater.home = lambda: os.path.join(base, 'app')  # nor the downloaded versions' folder
gui.pipeline.WORK = os.path.join(base, 'work')  # use the test's own workspace lock
os.makedirs(os.path.join(base, 'keys'))
json.dump({'keys': os.path.join(base, 'keys')}, open(settings, 'w'))  # an empty key folder: the user's keys are never looked at
try:
    # a saved layout whose window won't load: the modern window comes up instead, and the setting goes back to it
    broken = os.path.join(base, 'Broken.qml')
    open(broken, 'w').write('import QtQuick\nWindow { nothing here }\n')
    json.dump({'keys': os.path.join(base, 'keys'), 'layout': 'simple'}, open(settings, 'w'))
    real_window_qml = gui.window_qml
    gui.window_qml = lambda layout: QUrl.fromLocalFile(broken) if layout == 'simple' else real_window_qml(layout)
    qt, engine, state = gui.create(['test'])
    gui.window_qml = real_window_qml
    assert engine.rootObjects() and engine.rootObjects()[0].objectName() != 'simple', 'a broken layout left the app without a window'
    assert json.load(open(settings))['layout'] == 'modern'
    warnings.clear()  # the broken file's own
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
    for parts in ('full', 'binary', 'shaders', 'both'):
        state.setOutputFormat('homebrew')
        state.setHomebrewParts(parts)
        settle('Homebrew ' + parts)
        assert state.outputFormat == 'homebrew' and state.homebrewParts == parts
        saved = json.load(open(settings))
        assert saved['output_format'] == 'homebrew' and saved['homebrew_parts'] == parts
    for parts in ('binary','both'):
        state.setOutputFormat('wup-update')
        state.setWupUpdateParts(parts)
        settle('WUP update '+parts)
        assert state.outputFormat == 'wup-update' and state.wupUpdateParts == parts
        saved = json.load(open(settings))
        assert saved['output_format'] == 'wup-update' and saved['wup_update_parts'] == parts
    with mock.patch.object(gui.QFileDialog, 'getOpenFileName', return_value=(os.path.join(base,'original.base.json'),'')):
        state.chooseWupBase()
    assert state.hasWupBase and state.wupBaseText.endswith('original.base.json')
    assert json.load(open(settings))['wup_base_manifest'].endswith('original.base.json')
    state.clearWupBase()
    assert not state.hasWupBase and 'wup_base_manifest' not in json.load(open(settings))
    state.setOutputFormat('wup')
    settle('returning to WUP')
    # Selected save folders must remain readable, including names with no spaces.
    save_name = '20520623-074459_Animal_Crossing_New_Leaf_Checkpoint_backup_for_import'
    state.set(saveText=save_name, hasSave=True)
    for width, height in ((960, 540), (1280, 720), (1600, 900)):
        win.setWidth(width)
        win.setHeight(height)
        settle('the save row resized')
        subtitle = next(p for p in items(QQuickWindow.contentItem(win)) if p.property('text') == save_name)
        assert not subtitle.property('truncated'), 'the selected save folder is cut off'
        assert subtitle.y() + subtitle.height() <= subtitle.parentItem().height(), 'save text overlaps the next row'
    state.set(saveText=gui.NO_SAVE, hasSave=False)
    win.setWidth(1280)
    win.setHeight(720)
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
              cards=[dict(path='E:\\', label='E:\\    set up for a Wii U'), dict(path='F:\\', label='F:\\    removable drive')])
    for page in ('games', 'help', 'build'):
        win.setProperty('page', page)
        settle(f'page {page}')
        assert named_pills(f'page {page}')
    win.setProperty('page', 'games')
    games_page = next(p for p in items(QQuickWindow.contentItem(win)) if p.property('viewMode') is not None)
    games_page.setProperty('viewMode', 'compat')
    settle('compat list view')
    assert named_pills('compat list view')
    catalog = {g['program_id']: g for g in state.compatList}
    usa, eur = (catalog[i] for i in ('0004000000033500', '0004000000033600'))
    assert usa['name'] != eur['name'] and usa['wiiu_status'] == 'Plays' and eur['wiiu_status'] == 'Untested'
    assert usa['cemu_status'] == eur['cemu_status'] == 'Plays'
    miku, persona = (catalog[i] for i in ('0004000000148900', '0004000000123400'))
    assert (miku['wiiu_status'], miku['cemu_status']) == ('Plays', 'Limited')
    assert (persona['wiiu_status'], persona['cemu_status']) == ('Untested', 'Plays')
    for f in ('all', 'wiiu', 'cemu'):
        games_page.setProperty('platformFilter', f)
        settle(f'compat filter {f}')
        rows = [p for p in items(QQuickWindow.contentItem(win)) if p.objectName() == 'compatibilityRow']
        texts = [p.property('text') for row in rows for p in items(row) if p.isVisible()]
        assert (miku['name'] in texts) == (f != 'cemu')
        assert (persona['name'] in texts) == (f != 'wiiu')
        assert ('Wii U · ' + usa['wiiu_note'] in texts) == (f != 'cemu')
        assert ('Cemu · ' + usa['cemu_note'] in texts) == (f != 'wiiu')
    games_page.setProperty('viewMode', 'library')
    settle('library view restored')
    for result, badge in ((miku, 'Plays on Wii U'), (persona, 'Plays in Cemu')):
        state.game = SimpleNamespace(title_id=int(result['program_id'], 16))
        assert gui.pipeline.profile_for(state.game) is None
        state.refresh_note()
        assert state.badge == badge, state.note
    state.game = None
    win.setProperty('openFaq', 0)
    settle('an open answer')
    menu = win.findChild(QObject, 'cardMenu')
    menu.setProperty('visible', True)
    settle('the card menu')
    assert not win.findChild(QObject, 'pages').property('enabled'), 'Tab reaches the pages behind the card menu'
    menu.setProperty('visible', False)
    state.set(hasGame=True, hasArt=True, artOwn=True, hasIconArt=True, hasBezel=True, hasPadArt=True, hasGameSound=True, artText='A picture.png', iconText='An icon.png',
              bezelText='A bezel.png', padArtText='A GamePad picture.png', gameSoundText=gui.CUSTOM)  # every line of the art menu
    menu = win.findChild(QObject, 'artMenu')
    art_pill = next(p for p in items(QQuickWindow.contentItem(win)) if p.objectName() == 'pill' and p.property('accessibleName') == 'Art and icon')
    QMetaObject.invokeMethod(menu, 'openAt', Q_ARG('QVariant', art_pill))  # as the pill does: under it, its first line focused
    settle('the art menu')
    assert menu.property('visible') and not win.findChild(QObject, 'pages').property('enabled'), 'Tab reaches the pages behind the art menu'
    menu.setProperty('visible', False)
    state.set(hasGame=False, hasArt=False, artOwn=False, hasIconArt=False, hasBezel=False, hasPadArt=False, hasGameSound=False, artText=gui.NO_ART, iconText=gui.NO_ART,
              bezelText=gui.NO_BEZEL, padArtText=gui.NO_PAD_ART, gameSoundText=gui.NO_GAME_SOUND)
    stages = ['Reading', 'Recompiling', 'Packing']
    for view in ('building', 'done', 'failed', 'form'):
        state.set(view=view, stages=stages, stage=1, failed=view == 'failed', building=view == 'building', resultText='x', log='a\nb')
        win.setProperty('showLog', view == 'failed')
        settle(f'view {view}')
    # Build with nowhere to put the game (#29): "Put it on" starts under the window's edge, and comes into view, whole, under the message
    column = win.findChild(QObject, 'buildColumn')
    state.setTarget('card')
    state.st['card'] = ''
    settle('no card chosen')
    card = next(i for i in items(QQuickWindow.contentItem(win)) if i.property('text') == 'Put it on').parentItem()
    assert card.mapToItem(column, 0, 0).y() >= column.property('height'), '"Put it on" is in view already: this check shows nothing'
    real_otp_ok, gui.otp_ok, state.game = gui.otp_ok, lambda path: True, object()
    try:
        state.build()
        settle('Build with nowhere to put the game')
    finally:
        gui.otp_ok, state.game = real_otp_ok, None
    assert state.dialog['title'] == 'Where should it go?', state.dialog
    top = card.mapToItem(column, 0, 0).y()
    assert 0 <= top and top + card.property('height') <= column.property('height'), f'"Put it on" is not all in view: {top}'
    assert column.property('contentY') <= column.property('contentHeight') - column.property('height'), 'scrolled past the end'
    state.answer(True)
    column.setProperty('contentY', 0)
    settle('the message closed')
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
    # Downloaded art supplies startup screens; neither a SteamGridDB icon nor a fallback cover replaces the default menu icon.
    made = {n: os.path.join(base, 'covers', n) for n in ('sgdb_0000000000000001_banner.png', 'sgdb_0000000000000001_icon.png', 'AAAE.jpg')}
    os.makedirs(os.path.join(base, 'covers'))
    for path in made.values():
        img.save(path)
    banner, square, cover = made.values()
    picked = type('Game', (), dict(title_id=1, product='CTR-P-AAAE', icon=[[(0, 255, 0)] * 48 for _ in range(48)]))()
    gui.covers.cover = lambda product, folder: cover

    def pick(found, key='a key'):
        def site(name, key, folder, title_id, also=None):
            if found is None:
                raise gui.covers.BadKey
            return found
        gui.covers.steamgriddb = site
        state.st['sgdb_key'] = key
        state.game, state.art, state.icon_art = picked, None, None
        state.fetch_cover(picked, 'A game')
        return state.art, state.icon_art, state.artText

    assert pick(dict(banner=banner, icon=square)) == (banner, None, gui.FROM_SGDB) and state.iconText == gui.NO_ICON_ART
    assert pick(dict(banner=banner, icon=None)) == (banner, None, gui.FROM_SGDB) and state.iconText == gui.NO_ICON_ART
    assert pick(dict(banner=None, icon=square)) == (cover, None, gui.FROM_TDB) and state.iconText == gui.NO_ICON_ART
    assert pick(dict(banner=None, icon=None)) == (cover, None, gui.FROM_TDB) and state.iconText == gui.NO_ICON_ART
    preview = QImage.fromData(gui.base64.b64decode(state.iconArtUrl.split(',', 1)[1]))
    assert (preview.width(), preview.height()) == (128, 128) and state.tileUrl == state.iconArtUrl
    assert preview.pixelColor(64, 64).getRgb() == (0, 255, 0, 255) and 0 < preview.pixelColor(2, 2).green() < 255, 'the preview lost the default VC frame'
    assert pick(dict(banner=banner, icon=square), key='') == (cover, None, gui.FROM_TDB), 'SteamGridDB asked without a key'
    assert pick(None) == (cover, None, gui.FROM_TDB) and 'SteamGridDB' in state.dialog['title']
    state.answer(True)
    assert pick(None) == (cover, None, gui.FROM_TDB) and not state.dialog, 'a key turned down is said once, not for every game'

    # Legacy migration recognizes only this game's exact downloader filenames in its managed directory, without deleting them.
    gamekey = f'{picked.title_id:016x}'
    for cached in (square, cover):
        state.st.setdefault('icon_art', {})[gamekey] = cached
        state.set_art(banner, state.stored_icon(picked), remember=False)
        assert state.icon_art is None and state.iconText == gui.NO_ICON_ART
        assert state.st['icon_art'][gamekey] == cached and os.path.isfile(cached)
    for custom in (art, os.path.join(base, os.path.basename(square)), os.path.join(base, 'covers', 'my_icon.png'),
                   os.path.join(base, 'covers', 'sgdb_0000000000000002_icon.png')):
        state.st['icon_art'][gamekey] = custom
        assert state.stored_icon(picked) == custom, 'a custom or uncertain path was treated as a downloaded icon'
    # An explicit choice of even a cached picture gets provenance and survives reopening, fetching, and a new banner.
    gui.QFileDialog.getOpenFileName = lambda *a, **k: (square, '')
    state.chooseIcon()
    assert state.stored_icon(picked) == square and json.load(open(settings))['icon_picked'][gamekey] == square
    state.set_art(None, state.stored_icon(picked), remember=False)
    state.cover_ready(gamekey, banner, '')
    assert state.icon_art == square and state.iconText == gui.CUSTOM
    state.removeIcon()
    assert gamekey not in state.st['icon_picked'] and state.icon_art is None

    # a new key fetches the open game's art again, unless the player chose its picture or removed it on purpose
    asked_again = threading.Event()
    fetch_cover = state.fetch_cover
    state.fetch_cover = lambda *a: asked_again.set()
    gui.QInputDialog.getText = lambda *a, **k: (' another key ', True)
    state.set_art(None, removed=True)  # removed: '' is remembered for the game
    state.setArtKey()
    assert state.st['sgdb_key'] == 'another key' and state.artKeySet and not asked_again.wait(0.3), 'art removed on purpose came back'
    state.set_art(art)  # the player's own picture
    state.setArtKey()
    assert not asked_again.wait(0.3) and state.art == art, "the player's own picture was dropped"
    own_icon = os.path.join(base, 'icon.png')
    img.save(own_icon)
    state.set_art(cover, own_icon)  # GameTDB's cover with an icon the player picked: the cover is looked up again, the icon stays
    state.setArtKey()
    assert asked_again.wait(5), "GameTDB's cover wasn't looked up again with the new key"
    assert state.icon_art == own_icon, 'a new key dropped the icon the player picked'
    gui.QInputDialog.getText = lambda *a, **k: ('', True)
    state.setArtKey()
    assert state.st['sgdb_key'] == '' and not state.artKeySet
    state.fetch_cover = fetch_cover

    # An icon picture of its own. Each line says what its own picture is; choosing startup art keeps the selected icon;
    # removing the icon keeps the art; an icon picked with no art leaves the game's art to be fetched, and fetched art keeps that icon.
    title = f'{picked.title_id:016x}'
    chosen = iter([art, cover, own_icon, art])
    gui.QFileDialog.getOpenFileName = lambda *a, **k: (next(chosen), '')
    state.set_art(None, remember=False)
    state.chooseIcon()
    assert state.icon_art == art and state.art is None and state.hasIconArt and (state.iconText, state.artText) == (gui.CUSTOM, gui.NO_ART)
    assert title not in state.st['art'], 'an icon picked with no art marked the art as removed on purpose'
    state.chooseArt()
    assert state.art == cover and state.icon_art == art and (state.artText, state.iconText) == (gui.FROM_TDB, gui.CUSTOM)
    state.removeIcon()
    assert state.icon_art is None and state.art == cover and not state.hasIconArt and state.iconText == gui.NO_ICON_ART
    state.set_art(banner, square)  # a complete chosen icon is independent of the startup banner
    state.chooseIcon()  # own_icon
    state.set_art(None, own_icon)  # and with no art, what's fetched for it comes in beside that icon
    state.cover_ready(title, banner, square)
    assert (state.art, state.icon_art) == (banner, own_icon), 'fetched art took the place of the icon the player picked'
    state.set_art(banner, square)
    state.chooseArt()  # art
    assert (state.art, state.icon_art) == (art, square) and state.artText == gui.CUSTOM, "startup art replaced the selected icon"
    # the default art again: the player's pictures forgotten for the game, its art looked up again; offered after a removal too
    asked_again.clear()
    state.fetch_cover = lambda *a: asked_again.set()
    assert state.artOwn
    state.findArt()
    assert asked_again.wait(5) and (state.art, state.icon_art) == (None, None) and title not in state.st['art'] and not state.artOwn
    state.cover_ready(title, banner, square)
    assert (state.art, state.icon_art) == (banner, None) and not state.artOwn, 'fetched art replaced the default VC icon'
    state.removeArt()
    assert state.artOwn, 'art removed on purpose had no way back'
    state.fetch_cover = fetch_cover

    # the GamePad's bezel: chosen, kept for the game, removed
    gui.QFileDialog.getOpenFileName = lambda *a, **k: (cover, '')
    state.chooseBezel()
    assert state.bezel == cover and state.hasBezel and state.bezelText == gui.CUSTOM and json.load(open(settings))['pad_bezel'][title] == cover
    state.removeBezel()
    assert state.bezel is None and not state.hasBezel and state.bezelText == gui.NO_BEZEL and title not in json.load(open(settings))['pad_bezel']
    # the GamePad's start-up picture and the game's start-up sound: chosen, kept for the game, removed (the sound's file with it)
    state.choosePadArt()
    assert state.pad_art == cover and state.hasPadArt and state.padArtText == gui.CUSTOM and json.load(open(settings))['pad_art'][title] == cover
    state.removePadArt()
    assert state.pad_art is None and not state.hasPadArt and state.padArtText == gui.NO_PAD_ART and title not in json.load(open(settings))['pad_art']
    import wave
    sound = os.path.join(base, 'start.wav')
    with wave.open(sound, 'wb') as w:  # half a second of 48 kHz stereo
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(bytes(4 * 24000))
    gui.GAME_SOUNDS = os.path.join(base, 'boot_sounds')
    gui.QFileDialog.getOpenFileName = lambda *a, **k: (sound, '')
    state.chooseGameSound()
    kept = json.load(open(settings))['boot_sound'][title]
    assert kept == os.path.join(gui.GAME_SOUNDS, title + '.btsnd'), ('the game\'s sound went somewhere else, the sound for every game, say', kept)
    assert state.game_sound == kept and os.path.isfile(kept) and state.hasGameSound and state.gameSoundText == gui.CUSTOM, kept
    state.removeGameSound()
    assert state.game_sound is None and not state.hasGameSound and not os.path.exists(kept) and title not in json.load(open(settings))['boot_sound']
    assert state.gameSoundText == gui.NO_GAME_SOUND
    # a 3DS save to pack into the build: chosen (an emulator's folder, its save taken from data/00000001), kept for the game, removed
    emu = os.path.join(base, 'emu_save')
    os.makedirs(os.path.join(emu, 'data', '00000001'))
    open(os.path.join(emu, 'data', '00000001', 'main'), 'wb').write(b'save')
    gui.QFileDialog.getExistingDirectory = lambda *a, **k: emu
    state.bringSave()
    assert state.save_in == emu and state.hasSave and state.saveText == 'emu_save' and json.load(open(settings))['save_in'][title] == emu
    state.removeSave()
    assert state.save_in is None and not state.hasSave and state.saveText == gui.NO_SAVE and title not in json.load(open(settings))['save_in']
    gui.QFileDialog.getExistingDirectory = lambda *a, **k: os.path.join(base, 'keys')  # empty: not a save, said so
    state.bringSave()
    assert state.save_in is None and not state.hasSave and state.dialog
    state.answer(True)  # the message closed
    state.game = None

    # One folder of the 3DS's files (boot9.bin beside 0004009b and 000400db, as a tester kept them) fills both rows, from either Folder button.
    # The archives' import itself is faked (test_system_archives.py checks it): only which folders are taken is looked at here.
    three = os.path.join(base, '3DS files')
    for d in ('0004009b', '000400db'):
        os.makedirs(os.path.join(three, d))
    open(os.path.join(three, 'boot9.bin'), 'wb').write(bytes(65536))
    kept_env, kept_keys = os.environ.get('BOOT9_PATH'), state.st['keys']
    real_import, real_have = gui.dumps.import_system_archives, gui.dumps.have_system_archives
    imported = []
    gui.dumps.import_system_archives = lambda folder, out, work: imported.append(folder) or ['0004009b00010202']
    gui.dumps.have_system_archives = lambda folder: bool(imported)
    for button in (state.chooseSystem, state.chooseKeys):
        imported.clear()
        state.st['keys'], state.st['system'] = os.path.join(base, 'no keys'), ''
        gui.QFileDialog.getExistingDirectory = lambda *a, **k: three
        button()
        assert state.st['keys'] == three and imported == [three] and state.st['system'] == three, (button.__name__, state.st['keys'], imported)
        assert not state.dialog, button.__name__
    gui.dumps.import_system_archives, gui.dumps.have_system_archives = real_import, real_have
    state.st['keys'] = kept_keys
    if kept_env is None:
        os.environ.pop('BOOT9_PATH', None)
    else:
        os.environ['BOOT9_PATH'] = kept_env

    # A complete essential cache must not hide newly added files in the selected source.
    import struct
    source, cached = os.path.join(base, 'system source with spaces'), os.path.join(base, 'system cache')
    os.makedirs(source)
    def archive(folder, tid, body):
        with open(os.path.join(folder, tid + '.romfs'), 'wb') as file:
            file.write(struct.pack('<I', 0x28) + body)
    for tid in gui.dumps.ESSENTIAL_ARCHIVES:
        archive(source, tid, tid.encode() * 4)
    real_import(source, cached, base)
    archive(cached, '0004009b00010402', b'offline cached region data' * 4)
    cached_before = {name: open(os.path.join(cached, name), 'rb').read() for name in os.listdir(cached)}
    badword = '000400db00010302'
    assert real_have(cached) and not os.path.exists(os.path.join(cached, badword + '.romfs'))
    archive(source, badword, b'new bad-word fixture' * 4)
    dest = os.path.join(base, 'build destination')
    os.makedirs(dest)
    kept_st, kept_game = dict(state.st), state.game
    kept_flags = {key: state._s[key] for key in ('rootOk', 'target', 'building', 'view', 'updateState')}
    captured = []
    class BuildCaptured(Exception):
        pass
    def capture(argv, **kwargs):
        captured.append(argv)
        raise BuildCaptured  # stop before a process/thread/timer can be started
    try:
        state.game = SimpleNamespace(path=os.path.join(base, 'test.3ds'), encrypted=lambda: False)
        state.st.update(system=source, folder=dest, otp='unused test otp')
        with mock.patch.object(gui.pipeline, 'ARCHIVES', cached), mock.patch.object(gui, 'otp_ok', return_value=True), \
             mock.patch.object(gui.subprocess, 'Popen', side_effect=capture):
            state.set(rootOk=True, target='folder', building=False, view='form', updateState='')
            try:
                state.build()
            except BuildCaptured:
                pass
            assert captured and '--emulator' in captured[-1], 'GUI build dropped its selected system source once essentials were cached'
            selected = captured[-1][captured[-1].index('--emulator') + 1]
            assert selected == source and state.st['system'] == source
            assert badword in real_import(selected, cached, base)
            assert open(os.path.join(cached, badword + '.romfs'), 'rb').read() == open(os.path.join(source, badword + '.romfs'), 'rb').read()
            assert all(open(os.path.join(cached, name), 'rb').read() == body for name, body in cached_before.items())
            assert json.load(open(settings))['system'] == source
            # An unplugged source leaves cached/offline files and the selection intact.
            missing = os.path.join(base, 'unplugged source')
            state.st['system'] = missing
            state.set(building=False)
            try:
                state.build()
            except BuildCaptured:
                pass
            assert captured[-1][captured[-1].index('--emulator') + 1] == missing
            assert not real_import(missing, cached, base) and real_have(cached)
            assert state.st['system'] == missing and json.load(open(settings))['system'] == missing
            assert all(open(os.path.join(cached, name), 'rb').read() == body for name, body in cached_before.items())
            messages = []
            while not state.lines.empty():
                messages.append(state.lines.get_nowait())
            assert any(message and 'system folder is unavailable' in message for message in messages)
    finally:
        state.st, state.game = kept_st, kept_game
        state.set(**kept_flags)
        gui.save_settings(state.st)

    # Queue selection is a snapshot. Only a completed worker, including a failed launch, can advance it.
    kept_st = dict(state.st)
    kept_game, kept_update, kept_save = state.game, state.update, state.save_in
    kept_flags = {key: state._s[key] for key in ('rootOk', 'target', 'gameName', 'gameLanguage', 'gameMenu', 'menuStyle', 'menuShortcut', 'building', 'view', 'updateState')}
    card = os.path.join(base, 'test card')
    os.makedirs(card)

    def enqueue(name, target='folder', language='Console default'):
        state.game = SimpleNamespace(path=os.path.join(base, name + '.3ds'), name=name,
                                     title_id=0x0004000000054500, encrypted=lambda: False)
        state.set(gameName=name, target=target)
        state.setGameLanguage(language)
        state.enqueueBuild()

    def complete(result=None):
        if result:
            state.lines.put(result)
            state.poll()
            assert state.building, 'a result alone advanced the queue before the worker exited'
        state.lines.put(None)
        state.poll()

    def queue_status():
        return [item['status'] for item in state.queueItems]

    worker = mock.Mock()
    worker.poll.return_value = None
    try:
        state.st.update(system='', folder=dest, card=card, otp='first otp')
        state.set(rootOk=True, building=False, view='form', updateState='')
        with mock.patch.object(gui, 'otp_ok', return_value=True), mock.patch.object(state, 'find_system'), \
             mock.patch.object(gui.dumps, 'have_system_archives', return_value=True), \
             mock.patch.object(gui.pipeline, 'profile_for', side_effect=lambda game: {'folder': game.name, 'title_id': '000500003D000545'}), \
             mock.patch.dict(os.environ, THREEDII_BUILD='first work folder', BOOT9_PATH='first keys'), \
             mock.patch.object(gui.threading, 'Thread'), \
             mock.patch.object(gui.subprocess, 'Popen', side_effect=[worker, OSError('launch failed'), worker, worker]) as launch:
            state.update = SimpleNamespace(path='queued update.cia')
            state.save_in = 'queued save folder'
            state.setMenuShortcut('L+R')
            assert json.load(open(settings))['menu_shortcut'] == 'L+R'
            state.setMenuStyle('classic')
            enqueue('First', language='French')
            state.setMenuStyle('new')
            assert json.load(open(settings))['game_languages'][f'{state.game.title_id:016x}'] == 'French'
            enqueue('First', language='German')
            assert state.queuePending == 1 and state.dialog['title'] == 'That output is already queued'
            state.answer(True)
            state.update = state.save_in = None
            state.st['otp'] = 'second otp'
            state.setMenuShortcut('L+L')
            assert state.menuShortcut == 'L+R'
            state.setGameMenu(False)
            os.environ['THREEDII_BUILD'] = 'second work folder'
            os.environ['BOOT9_PATH'] = 'second keys'
            enqueue('Second')
            enqueue('Third', 'card')
            state.setGameMenu(True)
            state.setMenuShortcut('')
            enqueue('Remove me')
            state.removeQueued(3)
            enqueue('Fourth')
            state.set(gameName='Still editing', target='folder')
            state.startQueue()
            settle('queue started')
            assert state.buildName == 'First' and queue_status() == ['Building', 'Queued', 'Queued', 'Queued']
            first_args, first_env = launch.call_args.args[0], launch.call_args.kwargs['env']
            assert first_args[first_args.index('--game') + 1].endswith('First.3ds')
            assert first_args[first_args.index('--out') + 1] == dest
            assert first_args[first_args.index('--otp') + 1] == 'first otp'
            assert first_args[first_args.index('--update') + 1] == 'queued update.cia'
            assert first_args[first_args.index('--save') + 1] == 'queued save folder'
            assert first_args[first_args.index('--language') + 1] == 'French'
            assert first_args[first_args.index('--menu-shortcut') + 1] == 'L+R'
            assert first_args[first_args.index('--menu-style') + 1] == 'classic'
            assert first_env['THREEDII_BUILD'] == 'first work folder' and first_env['BOOT9_PATH'] == 'first keys'
            state.build()
            state.clearQueue()
            state.removeQueued(1)
            assert launch.call_count == 1 and state.queuePending == 3
            complete('@done first output')
            assert launch.call_count == 2 and state.buildName == 'Second'
            state.poll()
            assert launch.call_count == 3 and state.buildName == 'Third' and 'Copying' in state.stages
            assert queue_status() == ['Done', 'Failed', 'Building', 'Queued']
            assert state.queueItems[1]['detail'] == 'launch failed'
            third_args = launch.call_args.args[0]
            assert third_args[third_args.index('--card') + 1] == card and '--save' not in third_args and third_args[third_args.index('--language') + 1] == state.system_language
            assert '--no-menu' in third_args and '--menu-shortcut' not in third_args
            with mock.patch.object(gui, 'WIN', True), mock.patch.object(gui.subprocess, 'run'):
                gui.App.stop_for_close(state)
            complete()
            assert launch.call_count == 3 and not state.busy() and state.queuePending == 1, (
                launch.call_count, state.building, state.queueRunning, queue_status())
            assert queue_status() == ['Done', 'Failed', 'Cancelled', 'Queued']
            state.startQueue()
            assert launch.call_count == 4 and state.buildName == 'Fourth'
            assert all(flag not in launch.call_args.args[0] for flag in ('--no-menu', '--menu-shortcut', '--menu-style'))
            complete('@error build failed')
            settle('queue finished')
            assert not state.busy() and state.queuePending == 0 and state.queueItems[-1]['detail'] == 'build failed'
            assert state.gameName == 'Still editing'
            state.clearQueue()
            assert state.queueItems == []
            # A queued partial Homebrew export must keep its original format
            # and progress stages after the user changes the form back to WUP.
            state.setOutputFormat('homebrew')
            state.setHomebrewParts('both')
            enqueue('Homebrew update')
            job = state.jobs[0]
            state.setOutputFormat('wup')
            assert job.output_format == 'homebrew' and job.homebrew_parts == 'both'
            assert '--otp' not in job.argv and job.argv[job.argv.index('--homebrew-parts') + 1] == 'both'
            with mock.patch.object(gui.subprocess, 'Popen', return_value=worker):
                state.startQueue()
                assert state.stages == [name for name, _ in gui.pipeline.stages_for('homebrew', 'both') if name != 'Copying']
                complete('@done homebrew update')
            state.clearQueue()
            # Update choices and CLI flags belong to the queued job, not the form.
            for parts in ('binary', 'both'):
                state.setOutputFormat('wup-update')
                state.setWupUpdateParts(parts)
                state.st['wup_base_manifest'] = 'matching.base.json'
                enqueue('Native update')
                job = state.jobs[0]
                assert job.output.endswith(os.path.normcase('Native update_NativeWUP_Update'))
                assert job.argv[job.argv.index('--output-format') + 1] == 'wup-update'
                assert job.argv[job.argv.index('--wup-update-parts') + 1] == parts
                assert job.argv[job.argv.index('--wup-base-manifest') + 1] == 'matching.base.json'
                assert '--otp' in job.argv and '--homebrew-parts' not in job.argv
                state.setOutputFormat('wup')
                with mock.patch.object(gui.subprocess, 'Popen', return_value=worker):
                    state.startQueue()
                    assert state.stages == [name for name, _ in gui.pipeline.stages_for('wup-update', parts) if name != 'Copying']
                    complete('@done native update')
                assert 'same device' in state.resultText and 'delete that first' not in state.resultText
                state.clearQueue()
    finally:
        state.timer.stop()
        state.st, state.game, state.update, state.save_in = kept_st, kept_game, kept_update, kept_save
        state.set(**kept_flags)
        gui.save_settings(state.st)

    # the in-game menu (click the left stick), on or left out of the games it builds, kept in the settings
    state.setGameMenu(False)
    assert state.st['game_menu'] is False and not state.gameMenu and json.load(open(settings))['game_menu'] is False
    state.setGameMenu(True)
    assert state.gameMenu and json.load(open(settings))['game_menu'] is True

    def check_shortcut_picker(window):
        first = window.findChild(QObject, 'overlayFirstButton')
        second = window.findChild(QObject, 'overlaySecondButton')
        assert first is not None and second is not None
        assert first.property('currentText') == 'Default'
        assert QMetaObject.invokeMethod(first, 'activated', Q_ARG(int, state.menuButtons.index('L') + 1))
        settle('choose the first overlay button')
        assert second.property('currentText') == 'L Stick'
        assert QMetaObject.invokeMethod(second, 'activated', Q_ARG(int, 0))
        settle('choose the second overlay button')
        assert state.menuShortcut == 'L+R' and second.property('currentText') == 'R'
        assert QMetaObject.invokeMethod(first, 'activated', Q_ARG(int, 0))
        settle('reset the overlay shortcut')
        assert not state.menuShortcut and first.property('currentText') == 'Default'

    check_shortcut_picker(win)

    # The build folder (a tester's C: had no room): builds go to the one chosen, the old one's build files are offered for deletion and go only on
    # a yes, and Default comes back to the work folder. Nothing outside the build files is touched.
    kept_build, kept_work = os.environ.get('THREEDII_BUILD'), os.environ.get('THREEDII_WORK')
    work, roomy = os.path.join(base, 'work'), os.path.join(base, 'roomy')
    os.makedirs(os.path.join(work, 'builder_work'))
    os.makedirs(os.path.join(work, 'system_archives'))
    open(os.path.join(work, 'builder_work', 'big.bin'), 'wb').write(bytes(2 << 20))
    os.environ['THREEDII_WORK'] = os.environ['THREEDII_BUILD'] = work
    gui.QFileDialog.getExistingDirectory = lambda *a, **k: roomy
    state.chooseBuild()
    assert state.st['build_folder'] == os.path.normpath(roomy) and os.environ['THREEDII_BUILD'] == os.path.normpath(roomy)
    assert state.buildCustom and 'GB free' in state.buildText and 'Delete' in state.dialog['yes']
    state.answer(False)  # Keep
    assert os.path.isfile(os.path.join(work, 'builder_work', 'big.bin'))
    state.defaultBuild()
    assert not state.st['build_folder'] and os.environ['THREEDII_BUILD'] == work and not state.buildCustom and not state.dialog  # roomy has none
    state.chooseBuild()
    state.answer(True)  # Delete
    assert not os.path.exists(os.path.join(work, 'builder_work')) and os.path.isdir(os.path.join(work, 'system_archives'))
    state.defaultBuild()
    for k, v in (('THREEDII_BUILD', kept_build), ('THREEDII_WORK', kept_work)):
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v

    # the simple window (ui/Simple.qml): asked for, kept in the settings, and it loads cleanly with every app.<name> it uses on App, a
    # question shown in it and answered; it comes back to the modern one the same way
    used = set(re.findall(r'\bapp\.(\w+)', open(os.path.join(HERE, 'ui', 'Simple.qml'), encoding='utf-8').read()))
    missing = sorted(n for n in used if not hasattr(state, n))
    assert not missing, f'Simple.qml uses app.{missing}, which App lacks'
    asked = []
    state.layoutWanted.connect(asked.append)
    state.setLayout('simple')
    assert asked == ['simple'] and json.load(open(settings))['layout'] == 'simple', asked
    engine.load(gui.window_qml('simple'))
    simple = engine.rootObjects()[-1]
    assert simple.objectName() == 'simple' and not simple.property('visible'), 'the simple window did not load, or showed itself'
    settle('the simple window')
    state.setOutputFormat('homebrew')
    for parts in ('full', 'binary', 'shaders', 'both'):
        state.setHomebrewParts(parts)
        settle('simple Homebrew ' + parts)
    state.setOutputFormat('wup')
    settle('simple WUP')
    for parts in ('binary', 'both'):
        state.setOutputFormat('wup-update')
        state.setWupUpdateParts(parts)
        settle('simple WUP update ' + parts)
        assert state.wupUpdateParts == parts
    state.setOutputFormat('wup')
    settle('simple WUP after updates')
    check_shortcut_picker(simple)
    state.tell('A message', 'In the simple window.')
    settle('a message in the simple window')
    assert simple.findChild(QObject, 'dialog').property('visible'), 'the simple window did not show the message'
    state.answer(True)
    settle('the message answered')
    assert not simple.findChild(QObject, 'dialog').property('visible')
    # two questions waiting: its Yes button answers the first, and the second then stays up (the dialog follows app.dialog alone)
    dialog = simple.findChild(QObject, 'dialog')
    state.tell('First', 'The first question.')
    state.tell('Second', 'The second question.')
    settle('two messages in the simple window')
    assert dialog.property('title') == 'First'
    QMetaObject.invokeMethod(simple.findChild(QObject, 'yes'), 'click')
    settle('the first answered')
    assert dialog.property('visible') and dialog.property('title') == 'Second', (dialog.property('visible'), dialog.property('title'))
    QMetaObject.invokeMethod(simple.findChild(QObject, 'yes'), 'click')
    settle('the second answered')
    assert not dialog.property('visible') and not state.dialogs
    for dark in (True, False):
        state.set(dark=dark)
        settle(f'the simple window, dark {dark}')
    state.setLayout('bogus')
    state.setLayout('modern')
    assert asked == ['simple', 'modern'] and json.load(open(settings))['layout'] == 'modern', asked
    del engine  # before the App it shows, or its bindings read a gone app on the way out
    print('window ok')
finally:
    shutil.rmtree(base, ignore_errors=True)
