"""The files only you can supply: the keys and console data from your own Wii U (otp.bin) and your own 3DS (boot9.bin,
seeddb.bin). 3Dii U carries none of them and none of Nintendo's keys. Each check here says what's wrong with a file
before a build relies on it, without reading out, printing or storing what's in it. bundled() is the other half: what
make_app.py runs to make sure no such file ever ends up in the app it packs."""
import os
import hashlib
import struct

OTP_SIZE = 1024
OTP_COMMON_KEY = slice(0xE0, 0xF0)  # where the Wii U common key sits in the OTP (wiiubrew: bank 1), as phase0/tools/make_wup.py reads it
BOOT9_SIZES = (0x10000, 0x8000)  # the whole boot ROM, or only its protected half
# What a user's own console gives them, and what must never be shipped or committed.
NAMES = ('otp.bin', 'seeprom.bin', 'boot9.bin', 'boot11.bin', 'seeddb.bin', 'aes_keys.txt', 'movable.sed', 'keys.txt', 'mii_qr.key')
# Recognizable raw game/system dumps; other .bin files include required shader records.
DUMP_NAMES = ('code.bin', '.code', 'romfs.bin', 'exefs.bin', 'exheader.bin')
DUMP_EXTENSIONS = ('.3ds', '.cia', '.cci', '.cxi', '.romfs')

# Only this exact redistributable generated archive is allowed in a release.
PUBLIC_ARCHIVES = {'engine/third_party/system_archives/0004009b00010402.romfs': '4a4f770d617d93bffa1d6437d1caae0626b31b9f01c15d43aa3ed9bbb566b20b'}


def _size(path):
    return os.path.getsize(path) if path and os.path.isfile(path) else None


def check_otp(path):
    """None for a usable Wii U OTP dump, otherwise what's wrong with it"""
    size = _size(path)
    if size is None:
        return 'otp.bin is missing. It comes from your own Wii U\'s NAND backup.'
    if size != OTP_SIZE:
        return f'That otp.bin isn\'t a Wii U OTP dump: it should be exactly {OTP_SIZE} bytes.'
    with open(path, 'rb') as f:
        slot = f.read()[OTP_COMMON_KEY]
    if slot in (bytes(16), b'\xff' * 16):
        return 'That otp.bin has an empty common-key slot: dump it again from your own Wii U.'
    return None


def check_boot9(path):
    """None for a usable 3DS boot ROM dump, otherwise what's wrong with it"""
    size = _size(path)
    if size is None:
        return 'boot9.bin is missing. It comes from your own 3DS.'
    if size not in BOOT9_SIZES:
        return 'That boot9.bin isn\'t a 3DS boot ROM dump: it should be 64 KB (or 32 KB, its protected half).'
    return None


def check_seeddb(path):
    """None for a usable seed database (or none at all: only some digital games need one), otherwise what's wrong with it"""
    size = _size(path)
    if size is None:
        return None
    with open(path, 'rb') as f:
        head = f.read(4)
    count = struct.unpack('<I', head)[0] if len(head) == 4 else -1
    if count < 0 or size != 0x10 + count * 0x20:
        return 'That seeddb.bin isn\'t a seed database: its size doesn\'t match the number of seeds it says it holds.'
    return None


def check_key_folder(folder):
    """None if the 3DS key folder's boot9.bin and seeddb.bin are usable (seeddb.bin may be absent), otherwise what's wrong"""
    return check_boot9(os.path.join(folder, 'boot9.bin')) or check_seeddb(os.path.join(folder, 'seeddb.bin'))


def bundled(root):
    """Files named like private keys, console data or raw game dumps: an app or release must not contain these."""
    found = []
    for d, _, files in os.walk(root):
        for name in files:
            path = os.path.join(d, name)
            if name.lower() not in NAMES + DUMP_NAMES and not name.lower().endswith(DUMP_EXTENSIONS):
                continue
            relative = os.path.relpath(path, root).replace('\\', '/').lower()
            approved = PUBLIC_ARCHIVES.get(relative)
            if approved:
                with open(path, 'rb') as source:
                    if hashlib.sha256(source.read()).hexdigest() == approved: continue
            found.append(path)
    return found
