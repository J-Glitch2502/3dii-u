"""The pictures a check kept of each game, laid out to be looked at: contact_sheet.py <out folder> [game ...]
One <game>.png per game checked (all of them, or those named): a column for each leg and screen, in the order tools/check_ports.py took them.
Looking at them is the check: only then does check_ports.py --bless keep them as a good run."""
import glob
import os
import sys

from PySide6.QtCore import QRect, Qt
from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QPainter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H, PAD, LABEL = 400, 225, 8, 26


def sheet(kept, out):
    """kept: a game's check_shots/<game> folder, with a folder of tv_NN.png and pad_NN.png per leg"""
    cols = [(f'{leg.replace("_", " ")}, {name}', sorted(glob.glob(f'{kept}/{leg}/{screen}_*.png')))
            for leg in sorted(os.listdir(kept)) for screen, name in (('tv', 'TV'), ('pad', 'GamePad'))]
    rows = max(len(shots) for _, shots in cols)
    img = QImage(len(cols) * (W + PAD) + PAD, LABEL + rows * (H + PAD), QImage.Format_RGB888)
    img.fill(QColor(24, 24, 28))
    p = QPainter(img)
    p.setFont(QFont('Segoe UI', 11))
    p.setPen(QColor(235, 235, 235))
    for c, (label, shots) in enumerate(cols):
        x = PAD + c * (W + PAD)
        p.drawText(QRect(x, 0, W, LABEL), Qt.AlignCenter, label)
        for r, path in enumerate(shots):
            shot = QImage(path).scaled(W, H, Qt.KeepAspectRatio, Qt.SmoothTransformation)
            p.drawImage(x + (W - shot.width()) // 2, LABEL + r * (H + PAD), shot)
    p.end()
    img.save(out)


def main():
    out, names = sys.argv[1], sys.argv[2:]
    app = QGuiApplication([])  # for the labels' font
    os.makedirs(out, exist_ok=True)
    for kept in sorted(glob.glob(ROOT + '/games/*/check_shots/*')):
        name = os.path.basename(kept)
        if not names or name in names:
            sheet(kept, f'{out}/{name}.png')
            print(f'{out}/{name}.png')


if __name__ == '__main__':
    main()
