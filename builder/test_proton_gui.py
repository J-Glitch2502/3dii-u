"""Visible GUI regression check for Windows/Proton; isolated settings, no game or network needed.
Run with Windows Python or pass this script to the frozen Builder executable.
An optional second argument saves a screenshot at that path.
Unlike test_window.py, this uses the real display and rendering backend.
"""
import sys, os, json, tempfile, time
from pathlib import Path
import gui, shiboken6, PySide6
from PySide6.QtCore import QCoreApplication, QMetaObject, QObject, Qt
from PySide6.QtQuick import QQuickWindow

def settle():
    end=time.monotonic()+0.4
    while time.monotonic()<end:
        QCoreApplication.processEvents()
        time.sleep(0.01)

def children(item):
    yield item
    for c in item.childItems(): yield from children(c)

with tempfile.TemporaryDirectory() as temp:
    settings=os.path.join(temp,'settings.json')
    gui.settings_file=lambda:settings
    gui.updater.home=lambda:os.path.join(temp,'updates')
    keys=os.path.join(temp,'keys');os.mkdir(keys)
    json.dump({'keys':keys,'themeMode':'dark'},open(settings,'w'))
    app,engine,state=gui.create(['proton-visible-smoke'])
    assert engine.rootObjects()
    win=shiboken6.wrapInstance(shiboken6.getCppPointer(engine.rootObjects()[0])[0],QQuickWindow)
    warnings=[]
    engine.warnings.connect(lambda ws:warnings.extend(w.toString() for w in ws))
    gui.place(win);win.show();settle()
    assert win.isVisible() and win.isExposed()
    for name,page in [('Games page','games'),('Help page','help'),('Build page','build')]:
        target=next(o for o in children(win.contentItem()) if o.property('name')==name)
        assert QMetaObject.invokeMethod(target,'activated',Qt.ConnectionType.DirectConnection)
        settle();assert win.property('page')==page
    for output,parts in [('homebrew','both'),('wup-update','binary'),('wup-update','both'),('wup','both')]:
        state.setOutputFormat(output)
        if output=='wup-update':state.setWupUpdateParts(parts)
        settle();assert state.outputFormat==output
        assert json.load(open(settings))['output_format']==output
    assert gui.theme()['font'] in ('Arial','Tahoma','Segoe UI','Segoe UI Variable Text','Noto Sans','Cantarell'),gui.theme()['font']
    shot=win.grabWindow();assert not shot.isNull() and shot.width()>600
    if len(sys.argv)>1:
        assert shot.save(str(Path(sys.argv[1]).resolve()))
    simple=gui.QQmlApplicationEngine()
    simple.rootContext().setContextProperty('app',state)
    simple.rootContext().setContextProperty('T',gui.theme())
    simple.rootContext().setContextProperty('appIcon',gui.png_url(gui.draw_icon(512)))
    simple.load(gui.window_qml('simple'));assert simple.rootObjects()
    sw=shiboken6.wrapInstance(shiboken6.getCppPointer(simple.rootObjects()[0])[0],QQuickWindow)
    sw.show();settle();assert not sw.grabWindow().isNull()
    assert not warnings,warnings
    print('PROTON_VISIBLE_GUI_PASS',PySide6.__version__,gui.VERSION,'modern + simple, navigation, output settings, rendered screenshots',flush=True)
    sw.close();win.close();settle()
