"""Validate observed-cache parsing without compiling or distributing game shaders."""
import struct
import unittest
from append_shader_cache import records


def cache(revision, key=1, text=b'void main() {}', stage=0):
    return struct.pack('>7I', 0x53484443, revision, stage, 0, 0, key, len(text)) + text


class ObservedCacheTests(unittest.TestCase):
    def test_supported_revisions_preserve_stage_and_source(self):
        for revision in (2, 4, 5):
            for stage in (0, 1 << 24):
                with self.subTest(revision=revision, stage=stage):
                    found = records(cache(revision, stage=stage))
                    self.assertEqual(found[(bool(stage), 1)]['glsl'], 'void main() {}')

    def test_rejects_bad_headers_and_partial_records(self):
        good = cache(5)
        for bad in (b'', good[:7], cache(3), good[:-1], good + b'x', cache(5, stage=2 << 24)):
            with self.subTest(data=bad):
                with self.assertRaises(ValueError): records(bad)

    def test_duplicate_keys_cannot_silently_change_program(self):
        first = cache(5)
        self.assertEqual(len(records(first + first[8:])), 1)
        with self.assertRaises(ValueError): records(first + cache(5, text=b'different')[8:])


if __name__ == '__main__': unittest.main()
