"""Self-check for make_boot_sound.boot_sound: WAV files made here (16-bit stereo at 48 kHz, 8-bit mono at 24 kHz) come out as the samples
they hold, big-endian, with their second of silence; a long one is cut; a boot sound passes through; other files are turned down.
python tools/test_boot_sound.py"""
import os
import struct
import sys
import tempfile
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_boot_sound as mbs  # noqa: E402


def wav(path, channels, width, rate, raw):
    with wave.open(path, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(raw)
    return path


with tempfile.TemporaryDirectory() as tmp:
    frames = mbs.RATE // 2
    pairs = [(i * 7 % 30000 - 15000, -(i * 11 % 30000 - 15000)) for i in range(frames)]
    same = mbs.boot_sound(wav(os.path.join(tmp, 'same.wav'), 2, 2, mbs.RATE, b''.join(struct.pack('<hh', *p) for p in pairs)))
    assert struct.unpack('>II', same[:8]) == (2, frames) and len(same) == 8 + (frames + mbs.RATE) * 4
    assert struct.unpack('>%dh' % (frames * 2), same[8:8 + frames * 4]) == tuple(v for p in pairs for v in p) and not any(same[8 + frames * 4:])

    low = mbs.boot_sound(wav(os.path.join(tmp, 'low.wav'), 1, 1, mbs.RATE // 2, bytes([128 + 100, 128 - 100] * (frames // 2))))
    got = struct.unpack('>%dh' % (frames * 4), low[8:8 + frames * 8])
    assert struct.unpack('>II', low[:8]) == (2, frames * 2) and got[0::2] == got[1::2]          # twice the frames, the one channel on both sides
    assert got[0] == 100 << 8 and got[4] == -100 << 8 and got[2] == 0                           # its samples, and halfway between two of them

    long = mbs.boot_sound(wav(os.path.join(tmp, 'long.wav'), 1, 1, 8000, bytes([140]) * 8000 * (mbs.LONGEST + 1)))
    assert struct.unpack('>II', long[:8]) == (2, mbs.LONGEST * mbs.RATE), 'a long sound is read whole'

    kept = os.path.join(tmp, 'kept.btsnd')
    open(kept, 'wb').write(same)
    assert mbs.boot_sound(kept) == same
    for name, data in (('short.wav', b'RIFF\0\0\0\0WAVE'), ('quiet.wav', None), ('odd.btsnd', b'\0\0\0\7' + bytes(9)), ('loop.btsnd', struct.pack('>II', 2, 99) + bytes(8)),
                       ('words.txt', b'not a sound')):
        path = wav(os.path.join(tmp, name), 1, 2, mbs.RATE, bytes(200)) if data is None else os.path.join(tmp, name)
        if data is not None:
            open(path, 'wb').write(data)
        try:
            mbs.boot_sound(path)
            raise AssertionError(name + ' went through')
        except ValueError:
            pass
print('boot sound: ok')
