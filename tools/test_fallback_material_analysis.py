import json
from pathlib import Path
import struct
import tempfile
import unittest

from analyze_fallback_materials import analyze, describe, fnv64


def state(head=1, source=3, unused=0, stages=1):
    return struct.pack('<14I', head, 1, 0, 0, stages, 0, source | source << 4, 0, unused, 0x1100, 0, 0, 0, 0)


class MaterialAnalysisTests(unittest.TestCase):
    def test_live_configuration_is_not_part_of_material_program(self):
        a = state()
        b = bytearray(a)
        struct.pack_into('<I', b, 4, 1 | 2 << 6 | 5 << 8)  # UV selection and border wrap stay dynamic
        struct.pack_into('<I', b, 44, 1 | 4 | 4 << 3)  # fog and alpha test stay dynamic
        self.assertEqual(describe(a)['material'], describe(bytes(b))['material'])
        self.assertNotEqual(describe(a)['key'], describe(bytes(b))['key'])

    def test_unsupported_features_only_if_read(self):
        self.assertNotIn('procedural_source_requires_separate_support', describe(state(unused=6))['hazards'])
        self.assertIn('procedural_source_requires_separate_support', describe(state(source=6))['hazards'])
        self.assertIn('w_buffer_depth_not_written_by_generic_shader', describe(state(head=3))['hazards'])
        self.assertIn('exclusion_scissor_not_tested_by_generic_shader', describe(state(head=5))['hazards'])

    def test_reject_truncated_or_invalid_keys(self):
        for invalid in (b'', b'123', state()[:-4], state(stages=7)):
            with self.assertRaises(ValueError): describe(invalid)

    def test_capture_requires_complete_key_and_uniform_data(self):
        from decode_fallback_capture import decode
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp);key=state()
            header='[fbprobe] frame 12 key '+fnv64(key)+' rt 18000000 240x400 uber 1 lighting 0 bytes 4\n'
            complete=header+'[fbkey] 0 '+key.hex()+'\n[fbuni] 0 01020304\n[fbprobe] end\n'
            (p/'valid.log').write_text(complete)
            report=decode(p/'valid.log',p/'out')
            self.assertEqual(report['unique_states'],1)
            self.assertEqual(report['incomplete_or_invalid'],0)
            (p/'bad.log').write_text(complete.replace('[fbuni] 0','[fbuni] 4'))
            report=decode(p/'bad.log',p/'bad')
            self.assertEqual(report['unique_states'],0)
            self.assertEqual(report['incomplete_or_invalid'],1)

    def test_catalog_provenance_and_sampled_trace_are_distinct(self):
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp);a=state();b=state(source=1)
            (p/'observed.bin').write_bytes(struct.pack('<I', len(a))+a)
            (p/'catalog.bin').write_bytes(struct.pack('<I', len(b))+b)
            (p/'trace.log').write_text('[3dii-u draw] 0 fs='+fnv64(a)+'\n[3dii-u draw] 1 fs=0000000000000001\n')
            result=analyze([p/'observed.bin'],[],[p/'trace.log'],p/'out',[p/'catalog.bin'])
            self.assertEqual(result['observed_states'],1)
            self.assertEqual(result['preserved_catalog_states'],1)
            self.assertEqual(result['live_known_keys'],1)
            self.assertEqual(result['live_unknown_keys'],['0000000000000001'])
            self.assertTrue((p/'out/material-seeds.bin').is_file())


if __name__=='__main__': unittest.main()
