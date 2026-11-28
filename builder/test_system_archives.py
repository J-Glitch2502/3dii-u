"""Self-check for dumps.import_system_archives: the system archives come out of a NAND whose contents are RomFS images already (<n>.app.romfs), out of a folder of
<title id>.romfs files, and from wherever in an emulator's folders the user points (nand_titles). Run: python builder/test_system_archives.py"""
import os
from pathlib import Path
import struct
import sys
import tempfile
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dumps  # noqa: E402
from selftest import ncch


def romfs(path, body=b'x' * 64):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(struct.pack('<I', 0x28) + body)


with tempfile.TemporaryDirectory() as tmp:
    user = os.path.join(tmp, 'My 3DS files', 'user_directory')
    title = os.path.join(user, 'nand', dumps.NAND_ID, 'title')
    for tid in dumps.ESSENTIAL_ARCHIVES:
        romfs(os.path.join(title, tid[:8], tid[8:], 'content', '00000000.app.romfs'), tid.encode() * 4)
    junk = os.path.join(title, '0004009b', '00010402', 'content', '00000000.app.romfs')  # not a RomFS: left out
    os.makedirs(os.path.dirname(junk))
    open(junk, 'wb').write(b'\0' * 64)

    for pick in (os.path.dirname(user), user, os.path.join(user, 'nand'), os.path.join(user, 'nand', dumps.NAND_ID), title):
        assert dumps.nand_titles(pick) == title, (pick, dumps.nand_titles(pick))
    assert dumps.nand_titles(tmp) is None
    sd = os.path.join(tmp, 'SD')  # GodMode9's copy of title/0004009b off a 3DS: the user picks that folder, or the one holding it
    romfs(os.path.join(sd, '0004009b', '00010202', 'content', '00000000.app.romfs'))
    assert dumps.nand_titles(sd) == sd and dumps.nand_titles(os.path.join(sd, '0004009b')) == sd
    assert dumps.import_system_archives(os.path.join(sd, '0004009b'), os.path.join(tmp, 'from_sd'), tmp) == ['0004009b00010202']
    # GodMode9's other folder, title/000400db (the bad-word list): copied beside the first, both come from picking either
    romfs(os.path.join(sd, '000400db', '00010302', 'content', '00000000.app.romfs'))
    assert dumps.nand_titles(os.path.join(sd, '000400db')) == sd
    for pick in (os.path.join(sd, '0004009b'), os.path.join(sd, '000400db'), sd):
        assert dumps.import_system_archives(pick, os.path.join(tmp, 'both'), tmp) == ['0004009b00010202', '000400db00010302'], pick
    # what a game's code names of them: a title ID as two words side by side, either way round; half of one is nothing
    code = bytes(64) + struct.pack('<II', 0x00010302, 0x000400DB) + bytes(8) + struct.pack('<II', 0x0004009B, 0x00010202) + struct.pack('<I', 0x00010402)
    assert dumps.archives_named(code) == ['0004009b00010202', '000400db00010302'], dumps.archives_named(code)
    assert dumps.archives_named(struct.pack('<I', 0x00010302) * 8) == []

    out = os.path.join(tmp, 'archives')
    got = dumps.import_system_archives(user, out, tmp)
    assert sorted(got) == sorted(dumps.ESSENTIAL_ARCHIVES), got
    assert dumps.have_system_archives(out)
    assert open(os.path.join(out, '0004009b00010202.romfs'), 'rb').read()[4:] == b'0004009b00010202' * 4
    words = ('000400db00010302',)
    broken = os.path.join(out, words[0] + '.romfs')
    open(broken, 'wb').write(b'\0' * 64)
    assert dumps.have_system_archives(out) and not dumps.have_system_archives(out, words)
    assert words[0] in dumps.import_system_archives(sd, out, tmp)
    assert dumps.have_system_archives(out, words)

    again = os.path.join(tmp, 'again')  # a folder of <title id>.romfs, as the builder keeps them
    assert sorted(dumps.import_system_archives(out, again, tmp)) == sorted(dumps.ESSENTIAL_ARCHIVES + words)
    assert dumps.import_system_archives(out, out, tmp)  # importing a folder into itself leaves it as it was
    assert not dumps.have_system_archives(os.path.join(tmp, 'nothing'))

    # A stale or damaged .app can precede the usable content in copied NAND folders.
    raw = Path(tmp, 'Raw NAND')
    tid = words[0]
    content = raw / tid[:8] / tid[8:] / 'content'
    content.mkdir(parents=True)
    valid_romfs = struct.pack('<I', 0x28) + b'new word list' * 8
    valid_app = bytearray(ncch(valid_romfs))
    struct.pack_into('<Q', valid_app, 0x118, int(tid, 16))
    (content / '00000000.app').write_bytes(b'truncated')
    (content / '00000001.app').write_bytes(ncch(valid_romfs))  # wrong program ID
    bad_romfs = bytearray(ncch(bytes(len(valid_romfs))))
    struct.pack_into('<Q', bad_romfs, 0x118, int(tid, 16))
    (content / '00000002.app').write_bytes(bad_romfs)
    encrypted = bytearray(valid_app)
    encrypted[0x18F] = 0
    encrypted[0x1E0] ^= 1
    (content / '00000003.app').write_bytes(encrypted)
    last = content / '00000004.app'
    last.write_bytes(valid_app)
    with mock.patch.object(dumps.decrypt, 'to_plain', side_effect=dumps.decrypt.DecryptError('missing keys')):
        assert dumps.import_system_archives(str(raw), out, tmp) == [tid]
        assert Path(broken).read_bytes() == valid_romfs
        last.unlink()
        try:
            dumps.import_system_archives(str(raw), out, tmp)
        except dumps.decrypt.DecryptError as error:
            assert tid in str(error) and 'missing keys' in str(error)
        else:
            raise AssertionError('A locked archive must offer key recovery when nothing could be imported.')
        assert Path(broken).read_bytes() == valid_romfs
        assert not list(Path(out).glob('*.candidate*'))

    # A full-looking cache must not suppress the system files the player explicitly supplied.
    import pipeline
    cached = os.path.join(tmp, 'cached')
    os.makedirs(cached)
    for i in range(len(dumps.SYSTEM_ARCHIVES)):
        open(os.path.join(cached, f'unrelated-{i}.txt'), 'wb').close()

    class Game:
        path, kind, size = 'made-up.3ds', 'game', 0
        title_id, unique = 0x0004000000086300, 0x00086300
        product, name = 'CTR-P-TEST', 'Archive import test'
        encrypted = lambda self: False
        dlp_source = lambda self: None
        read_names = lambda self: None
        f = mock.Mock()

    class Imported(Exception):
        pass

    with mock.patch.object(pipeline, 'ARCHIVES', cached), \
            mock.patch.object(pipeline, 'BUILD', tmp), \
            mock.patch.object(pipeline, 'WORK', os.path.join(tmp, 'workspace')), \
            mock.patch.object(pipeline, 'WORK_GAMES', os.path.join(tmp, 'work')), \
            mock.patch.object(pipeline, 'profile_for', return_value=None), \
            mock.patch.object(pipeline.user_files, 'check_otp', return_value=None), \
            mock.patch.object(pipeline.shutil, 'disk_usage', return_value=mock.Mock(free=1 << 40)), \
            mock.patch.object(dumps, 'Dump', return_value=Game()), \
            mock.patch.object(dumps, 'extract_program'), \
            mock.patch.object(dumps, 'import_system_archives', side_effect=Imported) as requested, \
            mock.patch.object(sys, 'platform', 'win32'), \
            mock.patch.object(sys, 'argv', ['pipeline.py', '--game', 'made-up.3ds', '--otp', 'unused', '--out', tmp, '--emulator', user]):
        try:
            pipeline.main()
        except (Imported, SystemExit):
            pass
        requested.assert_called_once()
        assert requested.call_args.args[:2] == (user, cached)
with tempfile.TemporaryDirectory() as tmp:
    tid='0004009b00010402';archive=Path(tmp)/(tid+'.romfs')
    assert dumps.install_system_fallbacks(tmp,[]) == []
    assert not archive.exists()
    assert dumps.install_system_fallbacks(tmp,[tid]) == [tid]
    assert dumps.is_romfs(archive)
    assert dumps.install_system_fallbacks(tmp,[tid]) == []
    # Preserve an existing user archive, even when its bytes differ from ours.
    romfs(str(archive),b'user country-list data'*8)
    original=archive.read_bytes()
    assert dumps.install_system_fallbacks(tmp,[tid]) == []
    assert archive.read_bytes()==original
    archive.unlink()
    try: dumps.install_system_fallbacks(tmp,[tid],str(Path(tmp)/'incomplete-engine'))
    except dumps.DumpError: pass
    else: raise AssertionError('A missing packaged asset must produce a clear error')

print('system archives: ok')
