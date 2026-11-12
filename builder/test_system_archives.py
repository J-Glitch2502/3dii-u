"""Self-check for dumps.import_system_archives: the system archives come out of a NAND whose contents are RomFS images already (<n>.app.romfs), out of a folder of
<title id>.romfs files, and from wherever in an emulator's folders the user points (nand_titles). Run: python builder/test_system_archives.py"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dumps  # noqa: E402


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

    again = os.path.join(tmp, 'again')  # a folder of <title id>.romfs, as the builder keeps them
    assert sorted(dumps.import_system_archives(out, again, tmp)) == sorted(dumps.ESSENTIAL_ARCHIVES)
    assert dumps.import_system_archives(out, out, tmp)  # importing a folder into itself leaves it as it was
    assert not dumps.have_system_archives(os.path.join(tmp, 'nothing'))
print('system archives: ok')
