"""Self-check for user_files: well-formed and broken stand-ins for otp.bin, boot9.bin and seeddb.bin (random or zero
bytes, never real keys), and bundled() finding them in a folder. Run: python builder/test_user_files.py"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import user_files as uf  # noqa: E402


def put(folder, name, data):
    path = os.path.join(folder, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    return path


with tempfile.TemporaryDirectory() as tmp:
    good = bytearray(1024)
    good[uf.OTP_COMMON_KEY] = os.urandom(16)
    assert uf.check_otp(put(tmp, 'otp.bin', bytes(good))) is None
    assert 'empty' in uf.check_otp(put(tmp, 'blank/otp.bin', bytes(1024)))
    assert '1024' in uf.check_otp(put(tmp, 'short/otp.bin', bytes(1000)))
    assert 'missing' in uf.check_otp(os.path.join(tmp, 'nowhere', 'otp.bin'))

    keys = os.path.join(tmp, 'keys')
    assert 'missing' in uf.check_key_folder(keys)
    put(keys, 'boot9.bin', os.urandom(0x10000))
    assert uf.check_key_folder(keys) is None  # no seeddb.bin is fine
    assert uf.check_boot9(put(tmp, 'half/boot9.bin', os.urandom(0x8000))) is None
    assert 'boot ROM' in uf.check_boot9(put(tmp, 'odd/boot9.bin', os.urandom(1234)))
    put(keys, 'seeddb.bin', struct.pack('<I', 2) + bytes(12) + os.urandom(0x40))
    assert uf.check_key_folder(keys) is None
    put(keys, 'seeddb.bin', struct.pack('<I', 3) + bytes(12) + os.urandom(0x40))
    assert 'seed' in uf.check_key_folder(keys)

    clean = os.path.join(tmp, 'app')
    put(clean, 'engine/tools/readme.txt', b'x')
    assert uf.bundled(clean) == []
    put(clean, 'engine/deep/SeedDB.bin', b'x')
    assert [os.path.basename(p) for p in uf.bundled(clean)] == ['SeedDB.bin']
    put(clean, 'engine/deep/Mii_QR.KEY', b'placeholder')
    assert {os.path.basename(p) for p in uf.bundled(clean)} == {'SeedDB.bin', 'Mii_QR.KEY'}

    # Release checks reject recognizable raw dumps anywhere in the engine, without
    # rejecting the shader records and resources the builder deliberately ships.
    release = os.path.join(tmp, 'release')
    for name in ('pipelines.bin', 'fs_123.bin', 'vs_123.vsf', 'vs_123.vsr',
                 'iconTex.tga', 'bench.rpx', 'import_system_archives.py'):
        put(release, 'engine/resources/' + name, b'synthetic resource')
    assert uf.bundled(release) == []
    raw = ('code.bin', '.code', 'ROMFS.bin', 'exefs.bin', 'exheader.bin',
           'game.3ds', 'game.CIA', 'game.cci', 'game.cxi', '0004009b00010202.romfs')
    expected = {os.path.normpath(put(release, 'engine/deep/private/' + name, b'synthetic dump')) for name in raw}
    assert set(uf.bundled(release)) == expected, 'raw game/system data would be released'

    # the SD card a build goes onto, checked before it starts: a locked card (Windows' error 19) is said in plain words
    import pipeline  # noqa: E402
    locked = OSError(None, 'The media is write protected', 'D:\\3dii-u', 19)
    locked.winerror = 19  # Exercise the Windows shape on POSIX hosts too.
    assert 'locked' in pipeline.card_error(locked), pipeline.card_error(locked)
    assert "couldn't be written to: Access is denied" in pipeline.card_error(OSError(13, 'Access is denied'))
    card = os.path.join(tmp, 'card')
    os.makedirs(card)
    pipeline.check_card(card)
    assert os.listdir(os.path.join(card, 'install')) == [], 'the check left its file on the card'
with tempfile.TemporaryDirectory() as tmp:
    from pathlib import Path
    public = Path(__file__).resolve().parents[1]/'third_party/system_archives/0004009b00010402.romfs'
    relative = next(iter(uf.PUBLIC_ARCHIVES))
    exact = put(tmp, relative, public.read_bytes())
    assert uf.bundled(tmp) == []
    put(tmp,relative,b'other archive with the approved filename')
    assert uf.bundled(tmp) == [exact]
    os.remove(exact)
    elsewhere = put(tmp,'engine/private/0004009b00010402.romfs',public.read_bytes())
    assert uf.bundled(tmp) == [elsewhere]
print('user files: ok')
