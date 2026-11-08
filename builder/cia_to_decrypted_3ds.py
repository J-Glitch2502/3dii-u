#!/usr/bin/env python3
"""
CIA to Decrypted .3DS Converter
Converts Nintendo 3DS CIA dumps into clean, fully decrypted .3ds (CCI) ROMs.
Supports FW 9.6+ SeedDB titles and older titles alike.

The builder's copy of the user's converter: makerom -ciatocci, then pyctr decrypts every partition in place with the keys in ~/.3ds
(boot9.bin, seeddb.bin). Changed from the original only in where it looks for makerom (bin/ beside this file) and in having the in-place
decryption as a function of its own, decrypt_in_place, which the builder also runs on encrypted .3ds dumps.
"""

import sys
import os
import re
import shutil
import subprocess

try:
    from pyctr.type.cci import CCIReader
    from pyctr.type.ncch import NCCHSection
    import pyctr.crypto.seeddb as sdb
except ImportError:
    print("ERROR: 'pyctr' is not installed.")
    print("Run: pip install pyctr pycryptodome")
    sys.exit(1)

# Default paths for makerom and key files
KNOWN_MAKEROM_PATHS = [
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", "makerom.exe" if os.name == "nt" else "makerom"),
    os.path.join(os.path.dirname(__file__), "makerom.exe"),
    os.path.join(os.path.dirname(__file__), "makerom-v0.19.0-win_x86_64", "makerom.exe"),
    shutil.which("makerom") or "",
]

def find_makerom():
    for p in KNOWN_MAKEROM_PATHS:
        if p and os.path.isfile(p):
            return p
    return None

def clean_game_name(filename):
    """
    Cleans dump names like:
    '0004000000030800 Mario Kart 7 (CTR-P-AMKE) (v2.4.0) (U).legit.cia'
    into: 'Mario Kart 7.3ds'
    """
    base = os.path.basename(filename)
    name = re.sub(r"\.cia$", "", base, flags=re.IGNORECASE)
    # Remove leading 16-hex Title ID (e.g., '0004000000030800 ')
    name = re.sub(r"^[0-9a-fA-F]{16}\s*", "", name)
    # Remove trailing dump tags (.piratelegit, .legit, etc.)
    name = re.sub(r"\.(piratelegit|legit|standard)", "", name, flags=re.IGNORECASE)
    # Remove product codes, versions, and region codes like (CTR-P-XXXX), (vX.X.X), (U), (W)
    name = re.sub(r"\s*\([A-Z0-9-]+\)\s*", " ", name)
    name = re.sub(r"\s*\(v[0-9.]+\)\s*", " ", name)
    name = re.sub(r"\s*\([A-Z,]+\)\s*", " ", name)
    # Collapse multiple spaces and trim
    name = re.sub(r"\s+", " ", name).strip()
    if not name:
        name = os.path.splitext(base)[0]
    return name + ".3ds"

def decrypt_in_place(out_3ds_path):
    """Step 2: full in-place partition decryption with pyctr (supporting SeedDB & 7.x/9.x keys)"""
    print("[2/3] Decrypting partitions (ExeFS, RomFS, ExHeader)...")
    try:
        with CCIReader(out_3ds_path) as cci:
            parts_to_decrypt = []
            for p_idx, ncch in cci.contents.items():
                region = cci.sections[p_idx]
                parts_to_decrypt.append((p_idx, region.offset, region.size, ncch.open_raw_section(NCCHSection.FullDecrypted)))

            with open(out_3ds_path, "r+b") as out_f:
                for p_idx, offset, size, stream in parts_to_decrypt:
                    size_mb = size // (1024 * 1024)
                    print(f"  -> Decrypting Partition {p_idx} ({size_mb} MB)...")
                    out_f.seek(offset)
                    remaining = size
                    chunk_size = 4 * 1024 * 1024
                    while remaining > 0:
                        to_read = min(remaining, chunk_size)
                        data = stream.read(to_read)
                        if not data:
                            break
                        out_f.write(data)
                        remaining -= len(data)
    except Exception as e:
        print(f"[!] Decryption error: {e}")
        return False
    return True

def verify(out_3ds_path):
    """Step 3: verification"""
    print("[3/3] Verifying decrypted ROM integrity...")
    try:
        with CCIReader(out_3ds_path) as cci:
            ncch = cci.contents[0]
            with ncch.exefs.open(".code") as cf:
                code_header = cf.read(16)
            is_nocrypto = ncch.flags.no_crypto
            romfs_entries = list(ncch.romfs._tree_root["contents"].keys()) if ncch.romfs else []
            print(f"  [+] NoCrypto Flag: {is_nocrypto}")
            print(f"  [+] .code Header:  {code_header.hex()} (Valid ARM code)")
            if romfs_entries:
                print(f"  [+] RomFS Root:    {romfs_entries[:6]}")
            print("[SUCCESS] Fully decrypted and ready to play!")
            return True
    except Exception as e:
        print(f"[!] Verification warning: {e}")
        return False

def convert_and_decrypt(cia_path, out_3ds_path=None, makerom_path=None):
    cia_path = os.path.abspath(cia_path)
    if not os.path.isfile(cia_path):
        print(f"[!] File not found: {cia_path}")
        return False

    if makerom_path is None:
        makerom_path = find_makerom()
    if not makerom_path:
        print("[!] makerom.exe not found! Please place makerom.exe beside this script or install to PATH.")
        return False

    if out_3ds_path is None:
        out_dir = os.path.dirname(cia_path)
        out_3ds_path = os.path.join(out_dir, clean_game_name(cia_path))

    print(f"\n=======================================================")
    print(f"Input:  {os.path.basename(cia_path)}")
    print(f"Output: {os.path.basename(out_3ds_path)}")
    print(f"=======================================================")

    # Step 1: Repackage CIA to CCI container using makerom
    print("[1/3] Converting CIA container to CCI (.3ds)...")
    cmd = [makerom_path, "-ciatocci", cia_path, "-o", out_3ds_path, "-target", "p"]
    proc = subprocess.run(cmd, capture_output=True, text=True, creationflags=0x08000000 if os.name == "nt" else 0)
    if proc.returncode != 0:
        print(f"[!] makerom failed: {proc.stderr or proc.stdout}")
        return False

    return decrypt_in_place(out_3ds_path) and verify(out_3ds_path)

def main():
    makerom = find_makerom()
    if not makerom:
        print("ERROR: Cannot find makerom.exe.")
        sys.exit(1)

    targets = []
    if len(sys.argv) > 1:
        for arg in sys.argv[1:]:
            if os.path.isdir(arg):
                for root, _, files in os.walk(arg):
                    for f in files:
                        if f.lower().endswith(".cia"):
                            targets.append(os.path.join(root, f))
            elif os.path.isfile(arg) and arg.lower().endswith(".cia"):
                targets.append(arg)
    else:
        # Check current working directory for .cia files
        cwd = os.getcwd()
        print(f"No files specified. Searching in: {cwd}")
        for f in os.listdir(cwd):
            if f.lower().endswith(".cia"):
                targets.append(os.path.join(cwd, f))

    if not targets:
        print("No .cia files found to convert.")
        print("Usage:")
        print("  python cia_to_decrypted_3ds.py [path/to/game.cia ...]")
        print("  Or drag and drop .cia files onto Convert_CIA_to_Decrypted_3DS.bat")
        return

    print(f"Found {len(targets)} file(s) to process.")
    success_count = 0
    for f in targets:
        if convert_and_decrypt(f, makerom_path=makerom):
            success_count += 1

    print(f"\nFinished: {success_count}/{len(targets)} files converted successfully.")

if __name__ == "__main__":
    main()
