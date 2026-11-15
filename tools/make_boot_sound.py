#!/usr/bin/env python3
"""Turns a sound file into the sound the Wii U plays while a game starts (meta/bootSound.btsnd): make_boot_sound.py <sound> [out.btsnd]
Without an output it goes where every build picks it up: boot_sound.btsnd beside the system archives folder (make_game_title.py). The
builder's window does the same from its Help page, into its own work folder.

The format, read off retail titles' own files: two big-endian words, 2 (play on the TV and the GamePad) and the frame the sound loops back to
when it ends, then 48 kHz stereo 16-bit big-endian samples. A sound that should play once gets a second of silence after it and loops in that.
A WAV file needs nothing; any other kind of sound file needs PyAV (pip install av), which the builder's app doesn't carry."""
import os
import struct
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = os.path.join(ROOT, 'games', 'boot_sound.btsnd')
RATE = 48000
LONGEST = 20  # seconds read of a WAV file: a game has started long before, and this file's own reader isn't fast


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


def decode_wav(path):
    """the same of a WAV file of 8- or 16-bit samples, with nothing but Python: the first LONGEST seconds of its first two channels, at 48 kHz
    by straight lines between samples"""
    with wave.open(path, 'rb') as w:
        channels, width, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        if width not in (1, 2) or not channels or not rate:
            raise ValueError('only WAV files of 8- or 16-bit samples can be read')
        raw = w.readframes(min(frames, rate * LONGEST))
    frames = len(raw) // (width * channels)
    flat = struct.unpack('<%dh' % (frames * channels), raw[:frames * channels * 2]) if width == 2 else [(b - 128) << 8 for b in raw[:frames * channels]]
    left, right = flat[0::channels], flat[1::channels] if channels > 1 else flat[0::channels]
    out = bytearray()
    for i in range(frames * RATE // rate):
        at = i * rate / RATE
        a, t = int(at), at - int(at)
        b = min(a + 1, frames - 1)
        out += struct.pack('<hh', round(left[a] + (left[b] - left[a]) * t), round(right[a] + (right[b] - right[a]) * t))
    return bytes(out)


def btsnd(pcm_le):
    """a boot sound that plays these samples once, then stays silent"""
    frames = len(pcm_le) // 4
    samples = struct.unpack('<%dh' % (frames * 2), pcm_le[:frames * 4])
    return struct.pack('>II', 2, frames) + struct.pack('>%dh' % len(samples), *samples) + bytes(RATE * 4)


def boot_sound(path):
    """the boot sound made of a sound file, or the file's own bytes if it is one already; ValueError says what's wrong with it"""
    with open(path, 'rb') as f:
        head = f.read(12)
    if path.lower().endswith('.btsnd'):
        if len(head) < 8 or struct.unpack('>I', head[:4])[0] > 2 or (os.path.getsize(path) - 8) % 4 or struct.unpack('>I', head[4:8])[0] * 4 > os.path.getsize(path) - 8:
            raise ValueError('this is not a Wii U boot sound')
        with open(path, 'rb') as f:
            return f.read()
    if head[:4] == b'RIFF' and head[8:12] == b'WAVE':
        try:
            pcm = decode_wav(path)
        except wave.Error as e:
            raise ValueError(f'this WAV file can\'t be read ({e})') from e
    else:
        try:
            pcm = decode(path)
        except ImportError as e:
            raise ValueError('only WAV files can be read here') from e
    if len(pcm) < RATE * 4 // 10:
        raise ValueError('no sound came out of that file')
    return btsnd(pcm)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT
    try:
        data = boot_sound(sys.argv[1])
    except ValueError as e:
        sys.exit(str(e))
    with open(out, 'wb') as f:
        f.write(data)
    print(f'{out}: {(len(data) - 8) / 4 / RATE - 1:.2f} s of sound, {os.path.getsize(out) >> 10} KB')


if __name__ == '__main__':
    main()
