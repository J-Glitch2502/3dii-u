#!/usr/bin/env python3
"""Packs a title folder into a WUP folder: make_wup.py <path to your own otp.bin> [output name] [title folder]. Run with Windows Python.
The common key goes to the packer over stdin only, and is never printed or stored."""
import os
import subprocess
import sys

COMMON_KEY_OFFSET = 0xE0  # wiiubrew: OTP bank 1

otp = sys.argv[1]
name = sys.argv[2] if len(sys.argv) > 2 else '3DS_Recomp_Benchmark'
title = sys.argv[3] if len(sys.argv) > 3 else 'title'
if os.path.getsize(otp) != 1024:
    sys.exit('That is not a Wii U otp.bin: it should be exactly 1024 bytes.')
with open(otp, 'rb') as f:
    f.seek(COMMON_KEY_OFFSET)
    key = f.read(16)
if len(key) != 16 or len(set(key)) < 8:
    sys.exit('The common key slot in that file looks empty.')

run = subprocess.run(['wsl', '-d', 'Ubuntu-22.04', '--exec', 'noble', 'python3', '-I', '/project/phase0/tools/pack_wup.py',
                      f'/project/phase0/{title}', f'/project/phase0/dist/{name}'],
                     input=key.hex(), capture_output=True, text=True, errors='replace')
output = run.stdout + run.stderr
if key.hex().lower() in output.lower():  # pack_wup never prints it, so this should be impossible
    sys.exit('Refusing to show output: it contained the key.')
print(output)
sys.exit(run.returncode)
