#!/usr/bin/env python3
"""Packs build/title into dist/Tomodachi_Life, the WUP folder for WUP Installer GX2: pack_game_wup.py <path to your otp.bin>.
Run with Windows Python. The common key goes to the packer over stdin only, and is never printed or stored."""
import os
import subprocess
import sys

COMMON_KEY_OFFSET = 0xE0  # wiiubrew: OTP bank 1

otp = sys.argv[1]
if os.path.getsize(otp) != 1024:
    sys.exit('That is not a Wii U otp.bin: it should be exactly 1024 bytes.')
with open(otp, 'rb') as f:
    f.seek(COMMON_KEY_OFFSET)
    key = f.read(16)
if len(key) != 16 or len(set(key)) < 8:
    sys.exit('The common key slot in that file looks empty.')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def local(path):  # paths as WSL saw them still work
    return ROOT + path[len('/project'):] if path.startswith('/project/') else path
run = subprocess.run([sys.executable, '-E', '-P', os.path.join(ROOT, 'phase0', 'tools', 'pack_wup.py'),
                      local(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(ROOT, 'build', 'title'), local(sys.argv[3]) if len(sys.argv) > 3 else os.path.join(ROOT, 'dist', 'Tomodachi_Life')],
                     input=key.hex(), capture_output=True, text=True, errors='replace')
output = run.stdout + run.stderr
if key.hex().lower() in output.lower():  # pack_wup never prints it, so this should be impossible
    sys.exit('Refusing to show output: it contained the key.')
print(output)
sys.exit(run.returncode)
