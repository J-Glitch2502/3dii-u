"""Native update identities, selected payloads and fixed-version reinstalls."""
from datetime import datetime, timezone, timedelta
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import wup_update as u
class UpdateTests(unittest.TestCase):
 def test_native_version_lookup_identity(self):
  self.assertEqual(u.native_base_id(0x000500003d7c0545,0x0004000000054500),0x000500001d054500)
  self.assertEqual(u.native_base_id(0x000500001d054500),0x000500001d054500)
  self.assertEqual(u.native_base_id(0x0005000010334455,0x0004000000030800),0x000500001d030800)
  for legacy in (0x000500003d7c0545,0x000500001d7c0545,0x0005000010334455):
   with self.assertRaisesRegex(ValueError,'legacy WUP'):u.native_base_id(legacy)
  for unrelated in (0x000500001018dc00,0x0005001010040100):
   with self.assertRaises(ValueError):u.native_base_id(unrelated,0x0004000000030800)
 def test_native_ids_keep_adjacent_games_distinct(self):
  ids=[u.native_base_id(0x000500003d000000,0x0004000000000000|(unique<<8)) for unique in range(0x10000)]
  self.assertEqual(len(set(ids)),0x10000)
  self.assertTrue(all(t&0xff==0 and ((t&0x0fffffff)|0x10000000)==(t&0xffffffff) for t in ids))
  self.assertEqual(ids[0x308],0x000500001d030800)
  self.assertNotEqual(ids[0x308],ids[0x309])
  for program in (0x0004000001000000,0x0004000000030801,0x0004000e00030800):
   with self.assertRaises(ValueError):u.native_base_id(0x000500003d000308,program)
 def test_fixed_version_ignores_clock_and_build_count(self):
  dates=[datetime(2000,1,1,tzinfo=timezone.utc),datetime(2026,11,27,tzinfo=timezone.utc),datetime(2200,1,1,tzinfo=timezone.utc)]
  for date in dates:
   for _ in range(5):self.assertEqual(u.update_build_info(date)[0],5)
  with self.assertRaises(ValueError):u.update_build_info(dates[0].replace(tzinfo=None))

 def fixture(self, root, inputs=None):
  base=root/'base'
  for folder in ('code','meta','content'): (base/folder).mkdir(parents=True)
  (base/'code/app.xml').write_text('<app><title_id type="hexBinary">000500001D054500</title_id><title_version type="hexBinary">0000</title_version><group_id type="hexBinary">00003344</group_id><os_version type="hexBinary">000500101000400A</os_version><app_type type="hexBinary">80000000</app_type></app>')
  (base/'meta/meta.xml').write_text('<menu><title_id type="hexBinary">000500001D054500</title_id><title_version>0</title_version></menu>')
  (base/'code/game.rpx').write_bytes(b'updated executable')
  (base/'code/cos.xml').write_text('<app/>')
  for name in ('bootMovie.h264','bootLogoTex.tga','iconTex.tga'): (base/'meta'/name).write_bytes(b'synthetic art')
  for name in ('code.bin','exheader.bin','romfs.bin',*u.SHADER_FILES): (base/'content'/name).write_bytes(name.encode())
  (base/'content/language.txt').write_text('1\n')
  (base/'content/no_menu').touch()
  (base/'content/menu_shortcut.txt').write_text('L+R\n')
  manifest=root/'base.base.json'
  u.remember_base(manifest,base,0x000500001d054500,inputs=inputs,contract='a'*64)
  return base,manifest

 def test_update_payload_settings_links_and_roundtrip(self):
  import json,subprocess
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);base,manifest=self.fixture(root)
   (base/'content/menu_enabled.txt').write_text('1\n')
   (base/'content/menu_shortcut.txt').write_text('ZR+R\n')
   (base/'content/language.txt').write_text('2\n')
   for parts in u.PARTS:
    out=root/parts
    info=u.make_update(base,out,parts,base_manifest=manifest,contract='a'*64)
    self.assertEqual(ET.parse(out/'code/app.xml').findtext('title_id'),'0005000E1D054500')
    self.assertEqual(ET.parse(out/'meta/meta.xml').findtext('title_id'),'000500001D054500')
    self.assertEqual(info['version'],5)
    for name in ('language.txt','menu_enabled.txt','menu_shortcut.txt'):
     self.assertEqual((out/'content'/name).read_bytes(),(base/'content'/name).read_bytes())
    self.assertFalse((out/'content/romfs.bin').exists())
    self.assertFalse((out/'content/no_menu').exists())
    links=json.loads((out/'.3dii-base-links.json').read_text())['files']
    self.assertEqual(links[u.BASE_MARKER],65)
    self.assertNotIn('language.txt',links)
    self.assertNotIn('menu_shortcut.txt',links)
    self.assertNotIn('no_menu',links)
    self.assertEqual('shaders.gsh' in links, parts=='binary')
    self.assertEqual((out/'content'/u.UPDATE_MARKER).read_bytes(),(base/'content'/u.BASE_MARKER).read_bytes())
    self.assertEqual(len((out/'content'/u.UPDATE_MARKER).read_bytes()),65)
    self.assertEqual((out/'content/menu_enabled.txt').read_bytes(),b'1\n')
    result=subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'phase0/tools/pack_wup.py'),str(out),str(root/(parts+'-wup'))],input='00'*16+'\n',text=True,capture_output=True)
    self.assertEqual(result.returncode,0,result.stderr)
    self.assertIn('round trip verified',result.stdout)
    u.make_update(base,out,parts,base_manifest=manifest,contract='a'*64)

 def test_native_base_reinstall_changes_content_ids(self):
  import subprocess
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);base,manifest=self.fixture(root)
   def pack():
    result=subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'phase0/tools/pack_wup.py'),str(base),str(root/'wup')],input='00'*16+'\n',capture_output=True,text=True)
    self.assertEqual(result.returncode,0,result.stderr)
    return {p.name for p in (root/'wup').glob('*.app')}
   old=pack()
   (base/'content/new-data.bin').write_bytes(b'a changed full base')
   u.remember_base(manifest,base,0x000500001d054500,contract='a'*64)
   self.assertNotEqual(old,pack())

 def test_settings_clear_old_presence_flags_and_shortcuts(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);base,manifest=self.fixture(root)
   (base/'content/no_menu').unlink()
   (base/'content/menu_enabled.txt').write_bytes(b'1\r\n')
   (base/'content/menu_shortcut.txt').unlink()
   u.make_update(base,root/'update','binary',base_manifest=manifest,contract='a'*64)
   self.assertEqual((root/'update/content/menu_enabled.txt').read_text(),'1\n')
   self.assertEqual((root/'update/content/menu_shortcut.txt').read_text(),'\n')
   (base/'content/no_menu').touch()
   (base/'content/menu_enabled.txt').write_bytes(b'0\r\n')
   u.make_update(base,root/'update','binary',base_manifest=manifest,contract='a'*64)
   self.assertEqual((root/'update/content/menu_enabled.txt').read_text(),'0\n')

 def test_changed_assets_updates_dlc_and_shaders_rejected_before_output(self):
  import json
  inputs={'program_id':'0004000000054500','update':{'version':3},'dlc':{'version':2}}
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);base,manifest=self.fixture(root,inputs)
   for name in ('code.bin','romfs.bin','exheader.bin'):
    file=base/'content'/name;original=file.read_bytes();file.write_bytes(b'different modded data')
    with self.assertRaisesRegex(ValueError,'assets differ'):
     u.make_update(base,root/'bad','both',base_manifest=manifest,inputs=inputs,contract='a'*64)
    self.assertFalse((root/'bad').exists());file.write_bytes(original)
   for changed in ({**inputs,'update':None},{**inputs,'dlc':{'version':3}}):
    with self.assertRaisesRegex(ValueError,'selection differs'):
     u.make_update(base,root/'bad','both',base_manifest=manifest,inputs=changed)
   with self.assertRaisesRegex(ValueError,'Shader code changed'):
    u.make_update(base,root/'bad','binary',base_manifest=manifest,contract='b'*64)
   u.make_update(base,root/'both','both',base_manifest=manifest,contract='b'*64)
   # Binary-only reuses the actual base shader sizes without compiling any.
   for name in u.SHADER_FILES: (base/'content'/name).unlink()
   u.make_update(base,root/'binary','binary',base_manifest=manifest,contract='a'*64)
   self.assertFalse((root/'binary/content/shaders.gsh').exists())

 def test_manifest_rejection_and_shader_contract(self):
  import json
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);base,manifest=self.fixture(root)
   with self.assertRaisesRegex(ValueError,'matching'):
    u.read_manifest(manifest,0x000500001d030800)
   info=json.loads(manifest.read_text());info['content']['romfs.bin']['size']+=1
   manifest.write_text(json.dumps(info))
   with self.assertRaises(ValueError):u.read_manifest(manifest,0x000500001d054500)
   manifest.write_text(json.dumps({'base_title_id':'000500001D054500','files':{'shaders.gsh':1,'shaders.idx':1}}))
   with self.assertRaisesRegex(ValueError,'Older shader-size'):u.read_manifest(manifest,0x000500001d054500)
  self.assertEqual(len(u.shader_contract()),64)

if __name__=='__main__': unittest.main()
