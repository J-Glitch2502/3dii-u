import tempfile
import unittest
from pathlib import Path
import package_audit


class PackageAuditTests(unittest.TestCase):
    def test_detects_old_overlay_file_and_wrong_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, engine = root/'source', root/'engine'
            for base in (source, engine):
                (base/'port').mkdir(parents=True)
                (base/'port/current.cpp').write_text('current code')
            self.assertEqual(package_audit.verify(source, engine, ['port']), 1)
            (engine/'port/old.cpp').write_text('obsolete code')
            with self.assertRaisesRegex(ValueError, 'unexpected: port/old.cpp'):
                package_audit.verify(source, engine, ['port'])
            (engine/'port/old.cpp').unlink()
            (engine/'port/current.cpp').write_text('another checkout')
            with self.assertRaisesRegex(ValueError, 'different: port/current.cpp'):
                package_audit.verify(source, engine, ['port'])
            (engine/'port/current.cpp').unlink()
            with self.assertRaisesRegex(ValueError, 'missing: port/current.cpp'):
                package_audit.verify(source, engine, ['port'])

    def test_individual_required_file_is_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            source, engine = Path(directory)/'source', Path(directory)/'engine'
            for base in (source, engine):
                (base/'builder').mkdir(parents=True)
                (base/'builder/multiplayer-defaults.json').write_text('{"host":""}')
            files = ['builder/multiplayer-defaults.json']
            self.assertEqual(package_audit.verify(source, engine, files), 1)
            (engine/files[0]).unlink()
            with self.assertRaisesRegex(ValueError, 'missing: builder/multiplayer-defaults.json'):
                package_audit.verify(source, engine, files)

    def test_ignores_python_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            source, engine = Path(directory)/'source', Path(directory)/'engine'
            for base in (source, engine):
                (base/'tools').mkdir(parents=True)
            (engine/'tools/__pycache__').mkdir()
            (engine/'tools/__pycache__/extra.pyc').write_bytes(b'cache')
            self.assertEqual(package_audit.verify(source, engine, ['tools']), 0)


if __name__ == '__main__':
    unittest.main()
