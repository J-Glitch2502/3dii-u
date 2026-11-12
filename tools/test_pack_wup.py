"""Self-check for phase0/tools/pack_wup.py: a made-up title whose files sit on the edges of the packer's block sizes, packed with a made-up
key. The packer proves its own work (it decrypts what it wrote and compares it with the files), so its exit code is the check.
python tools/test_pack_wup.py [folder to leave the title in]"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = ('<?xml version="1.0"?><app><title_id type="hexBinary" length="8">000500003D0000FF</title_id><group_id type="hexBinary" length="4">00003D00</group_id>'
       '<app_type type="hexBinary" length="4">80000000</app_type><os_version type="hexBinary" length="8">000500101000400A</os_version>'
       '<title_version type="hexBinary" length="2">0000</title_version></app>')
BLOCK = 0xFC00  # the data in one block of a hashed content
SIZES = (0, 1, 0x1F, 0x20, 0x21, BLOCK - 1, BLOCK, BLOCK + 1, 16 * BLOCK, 16 * BLOCK + 5, 3 << 20)


def make_title(title):
    files = {'code/app.xml': APP.encode(), 'code/cos.xml': b'<app/>', 'code/game.rpx': os.urandom(100001), 'meta/meta.xml': b'<menu/>',
             'meta/bootMovie.h264': os.urandom(0x8000), 'meta/bootLogoTex.tga': os.urandom(77), 'meta/iconTex.tga': os.urandom(0xFC00 * 2)}
    files.update({f'content/file{i:02d}.bin': os.urandom(n) for i, n in enumerate(SIZES)})
    for name, body in files.items():
        os.makedirs(os.path.dirname(os.path.join(title, name)), exist_ok=True)
        with open(os.path.join(title, name), 'wb') as f:
            f.write(body)


def main():
    with tempfile.TemporaryDirectory() as work:
        title = sys.argv[1] if len(sys.argv) > 1 else os.path.join(work, 'title')
        make_title(title)
        r = subprocess.run([sys.executable, '-E', '-P', os.path.join(ROOT, 'phase0', 'tools', 'pack_wup.py'), title, os.path.join(work, 'wup')],
                           input='000102030405060708090a0b0c0d0e0f', capture_output=True, text=True, errors='replace')
        assert r.returncode == 0 and 'round trip verified' in r.stdout, r.stdout[-800:] + r.stderr[-800:]
    print('pack_wup ok')


main()
