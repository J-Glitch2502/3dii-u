import struct
import unittest
from append_shader_pack import merged_index, read_index


def index(ps=(), vs=()):
    return struct.pack('>8I', 0x53484458, 1, len(ps), len(vs), 0, 0, 0, 0) + b''.join(struct.pack('>4I', *e) for e in (*ps, *vs))


class AppendPackTests(unittest.TestCase):
    def test_offsets_count_programs_not_key_aliases(self):
        old = index(ps=[(0, 1, 0, 0)], vs=[(0, 2, 0, 3), (0, 3, 0, 3)])
        extra = index(ps=[(0, 1, 1, 0), (0, 4, 0, 0)], vs=[(0, 5, 1, 15)])
        result, added = merged_index(old, extra, {3: 1, 6: 1}, {3: 2, 6: 2})
        _, ps, vs = read_index(result)
        self.assertEqual(added, [1, 1])
        self.assertEqual(ps, [(0, 1, 0, 0), (0, 4, 1, 0)])
        self.assertEqual(vs, [(0, 2, 0, 3), (0, 3, 0, 3), (0, 5, 2, 15)])

    def test_missing_program_is_rejected(self):
        with self.assertRaises(ValueError):
            merged_index(index(), index(vs=[(0, 2, 1, 3)]), {3: 0, 6: 0}, {3: 1, 6: 0})

    def test_truncated_and_wrong_version_index(self):
        for bad in (b'', index()[:-1], struct.pack('>8I', 0x53484458, 2, 0, 0, 0, 0, 0, 0)):
            with self.assertRaises(ValueError): read_index(bad)


if __name__ == '__main__': unittest.main()
