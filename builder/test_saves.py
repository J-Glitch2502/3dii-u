#!/usr/bin/env python3
"""saves.py on made-up folders: which folder of the one chosen holds the save (an emulator's, Checkpoint's newest backup, any other as it
is), a folder that can't be a save, and the save packed into a title's content without what Windows and macOS leave. test_saves.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import saves  # noqa: E402


def put(path, data=b'save'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


with tempfile.TemporaryDirectory() as tmp:
    emu = os.path.join(tmp, 'emu')
    put(os.path.join(emu, 'data', '00000001', 'main'))
    assert saves.the_save(emu) == os.path.join(emu, 'data', '00000001')
    cp = os.path.join(tmp, 'checkpoint')
    for when in ('20261113-120000', '20261114-050000'):
        put(os.path.join(cp, when, 'main'), when.encode())
    put(os.path.join(cp, 'Thumbs.db'))
    assert saves.the_save(cp) == os.path.join(cp, '20261114-050000'), 'not the newest backup'
    plain = os.path.join(tmp, 'plain')
    put(os.path.join(plain, 'system.dat'))
    put(os.path.join(plain, 'sub', 'more.bin'))
    put(os.path.join(plain, '.DS_Store'))
    assert saves.the_save(plain) == plain and saves.checked(plain) == (plain, 2)

    for bad, why in ((os.path.join(tmp, 'empty'), 'no files'), (os.path.join(tmp, 'big'), 'too big')):
        os.makedirs(bad)
        if why == 'too big':
            put(os.path.join(bad, 'huge'), bytes(saves.ROOM + 1))
        try:
            saves.checked(bad)
            raise AssertionError(f'a folder with {why} was taken for a save')
        except saves.SaveError:
            pass

    content = os.path.join(tmp, 'title', 'content')
    put(os.path.join(content, 'save_import', 'stale'))  # a build before's, gone
    target = saves.pack(plain, content)
    assert target == os.path.join(content, 'save_import')
    assert sorted(f for f, _ in saves.files_in(target)) == sorted(['system.dat', os.path.join('sub', 'more.bin')]), saves.files_in(target)
    assert not os.path.exists(os.path.join(target, '.DS_Store')) and not os.path.exists(os.path.join(target, 'stale'))
    assert open(os.path.join(saves.pack(cp, content), 'main'), 'rb').read() == b'20261114-050000'
print('saves: ok')
