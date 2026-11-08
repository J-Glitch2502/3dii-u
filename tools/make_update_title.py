#!/usr/bin/env python3
"""make_update_title.py <game> <version>: an update title folder, <out>/update, from the game's <out>/title: the code, the shaders and the meta
files under the game's update title id (0005000E...) at that version. Installed with WUP Installer GX2 over the base title (no uninstall), the
system runs these files in place of the base's and leaves the rest of the content and the save alone, as it does for a game's own updates.
Pack it like a title: pack_game_wup.py <otp> <out>/update dist/<Name>_update."""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'win'))
from build3ds import Game  # noqa: E402

UPDATE_APP_TYPE = '0800001B'  # the app type of a game's update title, not a game: the menu shows no second icon for it


def patch(text, tag, value):
    out, n = re.subn(r'(<%s [^>]*>)\s*[0-9A-Za-z]+\s*(<)' % tag, r'\g<1>%s\g<2>' % value, text, count=1)
    assert n == 1, tag
    return out


def main():
    g = Game(sys.argv[1])
    version = int(sys.argv[2])
    assert 1 <= version <= 0xFFFF, 'the version is a number from 1 to 65535, above the base title\'s 0'
    src, dst = g.out + '/title', g.out + '/update'
    shutil.rmtree(dst, ignore_errors=True)
    for d in ('code', 'content', 'meta'):
        os.makedirs(f'{dst}/{d}')
    app = open(f'{src}/code/app.xml', encoding='utf8').read()
    title_id = int(re.search(r'<title_id [^>]*>\s*([0-9A-Fa-f]+)\s*<', app).group(1), 16)
    assert title_id >> 32 == 0x00050000, 'the source title must be a base game title'
    update_id = f'{title_id | (0x0000000E << 32):016X}'
    app = patch(patch(patch(app, 'title_id', update_id), 'title_version', f'{version:04X}'), 'app_type', UPDATE_APP_TYPE)
    open(f'{dst}/code/app.xml', 'w', encoding='utf8', newline='\n').write(app)
    shutil.copyfile(f'{src}/code/cos.xml', f'{dst}/code/cos.xml')
    for n in os.listdir(f'{src}/code'):
        if n.endswith(('.rpx', '.rpl')):
            shutil.copyfile(f'{src}/code/{n}', f'{dst}/code/{n}')
    # The system maps /vol/content to the update's own content folder, not over the base's (2026-11-06: "cannot open fs:/vol/content/exheader.bin"
    # from an update holding only the shaders). Until the update's file table can point at the base's content, an update carries all of it.
    full = len(sys.argv) > 3 and sys.argv[3] == 'full'
    for n in os.listdir(f'{src}/content') if full else ('shaders.gsh', 'shaders.idx'):
        shutil.copyfile(f'{src}/content/{n}', f'{dst}/content/{n}')
    for n in os.listdir(f'{src}/meta'):
        if n == 'meta.xml':
            # meta.xml keeps the game's own title id: the system checks it against the base title at launch and refuses the game otherwise
            # (ACP "title id in meta.xml is incorrect", error 199-9999, 2026-11-06); only app.xml and the tmd carry the update id
            meta = open(f'{src}/meta/meta.xml', encoding='utf8').read()
            meta = patch(meta, 'title_version', str(version))
            open(f'{dst}/meta/meta.xml', 'w', encoding='utf8', newline='\n').write(meta)
        else:
            shutil.copyfile(f'{src}/meta/{n}', f'{dst}/meta/{n}')
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(dst) for f in fs)
    print(f'update title {update_id} version {version}: {dst}, {total >> 20} MB')


main()
