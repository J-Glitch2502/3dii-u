#!/usr/bin/env python3
"""Turns a sound file into the sound the Wii U plays while a game starts (meta/bootSound.btsnd): make_boot_sound.py <sound> [out.btsnd]
Without an output it goes where every build picks it up: boot_sound.btsnd beside the system archives folder (make_game_title.py).

The format, read off retail titles' own files: two big-endian words, 2 (play on the TV and the GamePad) and the frame the sound loops back to
when it ends, then 48 kHz stereo 16-bit big-endian samples. A sound that should play once gets a second of silence after it and loops in that.
Needs PyAV (pip install av) to read the sound file."""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = os.path.join(ROOT, 'games', 'boot_sound.btsnd')
RATE = 48000


def decode(path):
    """the file's sound as 48 kHz stereo 16-bit little-endian bytes"""
    import av
    out = bytearray()
    with av.open(path) as f:
        resampler = av.AudioResampler(format='s16', layout='stereo', rate=RATE)
        for frame in f.decode(audio=0):
            for r in resampler.resample(frame):
                out += bytes(r.planes[0])[:r.samples * 4]
        for r in resampler.resample(None):  # what the resampler still holds
            out += bytes(r.planes[0])[:r.samples * 4]
    return bytes(out)


def btsnd(pcm_le):
    """a boot sound that plays these samples once, then stays silent"""
    frames = len(pcm_le) // 4
    samples = struct.unpack('<%dh' % (frames * 2), pcm_le[:frames * 4])
    return struct.pack('>II', 2, frames) + struct.pack('>%dh' % len(samples), *samples) + bytes(RATE * 4)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT
    pcm = decode(sys.argv[1])
    if len(pcm) < RATE * 4 // 10:
        sys.exit('no sound came out of that file')
    with open(out, 'wb') as f:
        f.write(btsnd(pcm))
    print(f'{out}: {len(pcm) / 4 / RATE:.2f} s of sound, {os.path.getsize(out) >> 10} KB')


if __name__ == '__main__':
    main()
