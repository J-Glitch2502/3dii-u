#!/usr/bin/env python3
"""pipeline.console_learned on a made-up SD card: what a console learned is read once, kept for builds without the card, and a file cut
short is left out without stopping the build. test_learned.py"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pipeline as p  # noqa: E402

INFO = {'title_id': '000500003D000301'}


def fragment(key):
    return b'FS  ' + struct.pack('<I', len(key)) + key


def main():
    with tempfile.TemporaryDirectory() as d:
        p.WORK = os.path.join(d, 'work')  # as the standalone app has it: not the repository
        card = os.path.join(d, 'card')
        on_card = os.path.join(card, '3dii-u', INFO['title_id'].lower())
        os.makedirs(on_card)
        assert p.console_learned(INFO, 'g_akde', card) == []  # a card with nothing learned on it
        with open(on_card + '/learned.bin', 'wb') as f:
            f.write(fragment(b'one state') + fragment(b'another') + fragment(b'one state'))  # the console writes a state again after a restart
        logs = p.console_learned(INFO, 'g_akde', card)
        assert len(logs) == 1 and os.path.dirname(logs[0]) == os.path.join(p.WORK, 'learned', 'g_akde'), logs
        with open(logs[0], 'rb') as f:
            assert f.read() == struct.pack('<I', 9) + b'one state' + struct.pack('<I', 7) + b'another'  # each state once, as the generator reads them
        assert p.console_learned(INFO, 'g_akde', card) == logs  # the same card again adds nothing
        assert p.console_learned(INFO, 'g_akde', None) == logs  # and a build without the card still has it
        with open(on_card + '/learned.bin', 'ab') as f:
            f.write(b'????' + b'\0' * 12)  # cut short by a console switched off as it wrote
        assert p.console_learned(INFO, 'g_akde', card) == logs
    print('ok')


if __name__ == '__main__':
    main()
