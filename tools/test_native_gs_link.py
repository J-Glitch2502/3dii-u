"""Reject a silently disconnected generated geometry-shader table."""
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parent / 'win'))
from build3ds import validate_native_gs_symbols

class NativeGeometryLinkTests(unittest.TestCase):
    def test_generated_c_symbols(self):
        validate_native_gs_symbols('01000000 D gs_native_count\n01000004 R gs_native_table', True)

    def test_no_generated_table_keeps_interpreter_fallback(self):
        validate_native_gs_symbols('         w gs_native_count\n         w gs_native_table', False)
        with self.assertRaises(ValueError):
            validate_native_gs_symbols('         w gs_native_count\n         w gs_native_table', True)

    def test_anonymous_namespace_reference_is_rejected(self):
        for required in (False, True):
            with self.assertRaises(ValueError):
                validate_native_gs_symbols(' w _ZN3gpu12_GLOBAL__N_115gs_native_countE\n'
                                          '01000000 D gs_native_count\n01000004 R gs_native_table', required)

if __name__ == '__main__': unittest.main()
