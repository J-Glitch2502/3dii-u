"""Portable catalog inventory and build integration regressions; synthetic data."""
from pathlib import Path
import hashlib, importlib.util, json, struct, sys, tempfile, unittest, shutil
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parent))
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'builder'))
import observation_catalog as catalog


def inventory(root, files):
    directory=catalog.directory(root);entries={}
    for name,data in files.items():
        p=directory/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        entries[name]=dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
    (directory/'manifest.json').write_text(json.dumps(dict(version=1,files=entries)))


class CatalogTests(unittest.TestCase):
    def test_missing_changed_and_path_traversal_fail(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);name='games/a/shader_logs/fs_log.bin'
            with self.assertRaises(ValueError):catalog.validate(root)
            inventory(root,{name:struct.pack('<II',4,1024)})
            self.assertEqual(len(catalog.validate(root)['files']),1)
            p=catalog.directory(root)/name;p.write_bytes(struct.pack('<II',4,1026))
            with self.assertRaisesRegex(ValueError,'missing or changed'):catalog.validate(root)
            p.unlink()
            with self.assertRaises(ValueError):catalog.validate(root)
            m=catalog.directory(root)/'manifest.json';m.write_text(json.dumps(dict(version=1,files={'games/../shader_logs/x.bin':{}})))
            with self.assertRaisesRegex(ValueError,'Invalid shader observation path'):catalog.validate(root)

    def test_staging_preserves_local_data_and_prediction_identity(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);data=struct.pack('<II',4,1024)
            inventory(root,{'games/a/shader_logs/fs_log.bin':data,'games/a/shader_logs/confirmed.bin':data})
            logs=root/'dump/shader_logs';logs.mkdir(parents=True);(logs/'fs_log.bin').write_bytes(b'private')
            catalog.stage_own(root,'a',logs)
            self.assertEqual((logs/'fs_log.bin').read_bytes(),b'private')
            self.assertEqual((logs/'catalog_fs_log.bin').read_bytes(),data)
            self.assertEqual((logs/'preserved_catalog_confirmed.bin').read_bytes(),data)

    def test_source_build_uses_catalog_without_ignored_games_folder(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);data=struct.pack('<II',4,1024)
            inventory(root,{'games/a/shader_logs/fs_log.bin':data,'games/b/shader_logs/shared_fs.bin':data})
            source=Path(__file__).resolve().parent/'win/build3ds.py'
            shutil.copyfile(source.parent.parent/'fs_variants.py',root/'tools/fs_variants.py')
            spec=importlib.util.spec_from_file_location('catalog_test_build3ds',source);b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
            g=SimpleNamespace(dir=str(root/'dump'),name='a')
            b.ROOT=str(root);b.WORK=str(root/'work')
            with patch.object(b,'run',return_value=SimpleNamespace(stdout='',returncode=0)):
                b.prepare_logs(g,'unused-generator')
            logs=Path(g.dir)/'shader_logs'
            self.assertEqual((logs/'catalog_fs_log.bin').read_bytes(),data)
            self.assertEqual((logs/'other_catalog_b_shared_fs.bin').read_bytes(),data)
            self.assertFalse((root/'games').exists())
            (catalog.directory(root)/'games/b/shader_logs/shared_fs.bin').unlink()
            with patch.object(b,'run') as run:
                with self.assertRaises(ValueError):b.prepare_logs(g,'unused')
                run.assert_not_called()


if __name__=='__main__':unittest.main()
