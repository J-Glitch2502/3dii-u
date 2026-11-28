"""Missing input states fail even when a shader pack still has programs."""
from pathlib import Path
import json,struct,tempfile,unittest
from shader_coverage import check,fnv64

class CoverageTests(unittest.TestCase):
    def fixture(self,root,states,entries):
        logs=root/'logs';out=root/'output';logs.mkdir();(out/'glsl').mkdir(parents=True)
        (logs/'fs_log.bin').write_bytes(b''.join(struct.pack('<II',4,state) for state in states))
        (out/'glsl/list.txt').write_text('ps0.frag\nvs0.vert\n')
        (out/'shaders.idx').write_bytes(struct.pack('>8I',0x53484458,1,len(entries),0,0,0,0,0)+b''.join(struct.pack('>QII',fnv64(struct.pack('<I',state)),index,0) for state,index in entries))
        return logs,out

    def test_canonical_alias_does_not_count_as_a_loss(self):
        with tempfile.TemporaryDirectory() as t:
            logs,out=self.fixture(Path(t),[1024,1024|128|512],[(1024,0)])
            report=check(logs,out)
            self.assertEqual(report['distinct_input_states'],2);self.assertEqual(report['missing'],[])

    def test_removed_state_fails_with_report(self):
        with tempfile.TemporaryDirectory() as t:
            logs,out=self.fixture(Path(t),[1024,1026],[(1024,0)])
            with self.assertRaisesRegex(ValueError,'coverage regression'):check(logs,out)
            self.assertEqual(len(json.loads((out/'shader-coverage-report.json').read_text())['missing']),1)

    def test_out_of_range_and_duplicate_lookup_fail(self):
        for entries,reason in [([(1024,1)],'outside'), ([(1024,0),(1024,0)],'Duplicate')]:
            with self.subTest(reason=reason),tempfile.TemporaryDirectory() as t:
                logs,out=self.fixture(Path(t),[1024],entries)
                with self.assertRaisesRegex(ValueError,reason):check(logs,out)

    def test_pipelines_are_also_required(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);logs,out=self.fixture(root,[1024],[(1024,0)])
            p=root/'pipelines.bin';p.write_bytes(bytes(8)+struct.pack('<II',4,1026))
            with self.assertRaisesRegex(ValueError,'coverage regression'):check(logs,out,p)
            p.write_bytes(bytes(8)+struct.pack('<I',8))
            with self.assertRaisesRegex(ValueError,'pipeline fragment'):check(logs,out,p)

if __name__=='__main__':unittest.main()
