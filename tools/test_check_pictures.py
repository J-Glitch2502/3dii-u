#!/usr/bin/env python3
"""tools/check_ports.py's picture check, on made-up pictures: test_check_pictures.py. Needs PySide6, as the check does."""
import os
import sys
import tempfile

from PySide6.QtGui import QColor, QImage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_ports as c  # noqa: E402


def leg(t, colours):
    """a leg's screenshots as cemu_run.ps1 leaves them, each one flat colour and the same on both screens, and what the check says of them"""
    os.makedirs(t.data + '/shots/cemu', exist_ok=True)
    for i, colour in enumerate(colours):
        img = QImage(128, 72, QImage.Format_RGB888)
        img.fill(QColor(colour))
        for screen in ('tv', 'pad'):
            img.save(f'{t.data}/shots/cemu/{screen}_{i:02d}.png')
    return c.pictures(t, 'blank_save')


def main():
    with tempfile.TemporaryDirectory() as d:
        t = c.Target('game', '0', d, d.replace('\\', '/'))
        fault, note = leg(t, ['red', 'green', 'blue', 'white'])
        assert not fault and 'no good pictures' in note, (fault, note)
        assert c.bless(t) == '8 pictures kept as a good run'
        assert leg(t, ['red', 'green', 'blue', 'white']) == (None, "blank_save: pictures like a good run's")
        assert not leg(t, ['red', 'green', 'blue', 'black'])[0]  # one picture off: a frame caught at another moment
        assert leg(t, ['red', 'green', 'yellow', 'cyan'])[0]     # two pictures no good run has
        assert leg(t, ['red', 'red', 'red', 'green'])[0]         # nothing wrong in what's there, but two scenes of the good run never came
        QImage(8, 8, QImage.Format_RGB888).save(t.data + '/shots/cemu/tv_00.png')
        assert 'GamePad' in c.pictures(t, 'blank_save')[0]       # a screen with no pictures at all
    print('ok')


if __name__ == '__main__':
    main()
