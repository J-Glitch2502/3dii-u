"""Build a small native Wii U game update, leaving base content and saves installed."""
from datetime import datetime, timezone
from contextlib import contextmanager
import tempfile
from pathlib import Path
import json
import hashlib
import os
import shutil
import xml.etree.ElementTree as ET

# Manual WUP reinstalls with changed content IDs were verified on hardware
# while keeping native version 5. Timestamps label builds, never order updates.
UPDATE_VERSION = 5
PARTS = ('binary', 'both')
APP_TYPE = '0800001B'



def native_base_id(title_id, program_id=None):
    """Use a reserved 1D ID with a zero variation byte for native grouping.

    nn_vctl needs the top nibble 1 for patch lookup. System Settings groups
    saves with the last byte cleared, but groups software by its complete ID.
    Encode the 3DS game's unique number in bits 8..23; never truncate it or
    derive it by masking a legacy WUP ID (which can have custom test bits).
    """
    from wup_safety import check_title_id
    check_title_id(title_id)
    if title_id >> 32 != 0x00050000:
        raise ValueError('Expected a base game title ID')
    if program_id is not None:
        if program_id >> 32 != 0x00040000 or program_id & 0xff:
            raise ValueError('Expected a 3DS base game ID with a zero variation byte')
        unique = (program_id & 0xffffffff) >> 8
        if unique > 0xffff:
            raise ValueError('3DS unique number exceeds the reserved WUP ID range; refusing to truncate it')
        return 0x000500001d000000 | (unique << 8)
    if title_id & 0xff or (title_id & 0xff000000) != 0x1d000000:
        raise ValueError('This legacy WUP ID cannot group software and saves correctly. Rebuild the base from the original 3DS game; existing saves need migration to its corrected ID.')
    return title_id


SHADER_FILES = ('shaders.gsh', 'shaders.idx', 'fallback-shaders.bin')
OPTIONS = ('language.txt', 'menu_enabled.txt', 'menu_shortcut.txt', 'no_menu')
BASE_MARKER = '3dii-base-id.txt'
UPDATE_MARKER = '3dii-update-base-id.txt'
MANIFEST_VERSION = 2


def _digest(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(4 << 20), b''): digest.update(block)
    return dict(size=Path(path).stat().st_size, sha256=digest.hexdigest())


def _content(title):
    content = Path(title)/'content'
    result = {}
    for file in sorted(content.rglob('*')):
        if file.is_symlink(): raise ValueError('Title content must not contain symbolic links')
        if file.is_file():
            name = file.relative_to(content).as_posix()
            if name not in (*OPTIONS, BASE_MARKER, UPDATE_MARKER, '3dii-update.json'):
                result[name] = _digest(file)
    return result


def shader_contract(root=None):
    root = Path(root or os.environ.get('THREEDII_ROOT') or Path(__file__).resolve().parents[1])
    names = ['port/wiiu/fs_key.inc', 'port/wiiu/shadergen.h', 'port/wiiu/shadergen.cpp',
             'port/wiiu/gx2_backend.cpp', 'port/wiiu/latte_program.h', 'port/wiiu/uber_family.h',
             'third_party/3dsrecomp/runtime/gpu/gpu.h', 'third_party/3dsrecomp/runtime/gpu/pica.h',
             'third_party/3dsrecomp/runtime/fallback.h', 'tools/shadergen_main.cpp',
             'tools/shadergen_lists.cpp', 'tools/fallbackgen.cpp']
    digest = hashlib.sha256(b'3dii-shader-contract-v1')
    for name in names:
        digest.update(name.encode()+b'\0')
        digest.update((root/name).read_bytes())
    return digest.hexdigest()


def _identity(record):
    fields = {key:record[key] for key in ('format','base_title_id','content','shader_contract','inputs')}
    return hashlib.sha256(json.dumps(fields, sort_keys=True, separators=(',',':')).encode()).hexdigest()


def write_manifest(path, record):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, delete=False) as out:
        temporary = Path(out.name)
        json.dump(record, out, indent=2, sort_keys=True)
        out.write('\n')
    try: os.replace(temporary, path)
    finally: temporary.unlink(missing_ok=True)


def remember_base(path, title, base_id, *, inputs=None, contract=None, root=None):
    """Register the actual assets and base shaders; call before packing the base."""
    native_base_id(base_id)
    content = _content(title)
    if not all(name in content for name in ('code.bin','exheader.bin','romfs.bin',*SHADER_FILES[:2])):
        raise ValueError('The full base game assets and shaders are required for an update manifest')
    record = dict(format=MANIFEST_VERSION, base_title_id=f'{base_id:016X}', content=content,
                  inputs=inputs or {}, shader_contract=contract or shader_contract(root))
    record['identity'] = _identity(record)
    (Path(title)/'content'/BASE_MARKER).write_bytes((record['identity']+'\n').encode('ascii'))
    write_manifest(path, record)
    return record


def read_manifest(path, base_id):
    try:
        record = json.loads(Path(path).read_text(encoding='utf-8'))
        if record['format'] != MANIFEST_VERSION or int(record['base_title_id'],16) != base_id: raise ValueError()
        content = record['content']
        if not isinstance(content,dict) or not content: raise ValueError()
        for name, info in content.items():
            if (not isinstance(name,str) or not name or name.startswith('/') or '\\' in name or ':' in name
                    or any(part in ('','.','..') for part in name.split('/'))
                    or name in (*OPTIONS,BASE_MARKER,UPDATE_MARKER,'3dii-update.json')
                    or type(info['size']) is not int or not 0 <= info['size'] <= 0xffffffff
                    or len(info['sha256'])!=64 or any(c not in '0123456789abcdef' for c in info['sha256'])): raise ValueError()
        if not all(n in content for n in ('code.bin','exheader.bin','romfs.bin',*SHADER_FILES[:2])): raise ValueError()
        if not isinstance(record['inputs'],dict) or record['identity'] != _identity(record): raise ValueError()
        return record
    except (OSError,ValueError,KeyError,TypeError) as error:
        raise ValueError('Choose the matching .base.json saved beside a full WUP build made with this Builder. Older shader-size inventories cannot verify a base; rebuild the full base once.') from error


def verify_base(title, record, parts, *, inputs=None, contract=None):
    if inputs is not None and inputs != record['inputs']:
        raise ValueError('The game/update/DLC selection differs from the base build. Choose its matching inputs or make a new full WUP base.')
    current = _content(title)
    assets = lambda files: {n:v for n,v in files.items() if n not in SHADER_FILES and not n.startswith('save_import/')}
    expected, actual = assets(record['content']), assets(current)
    differences = sorted(n for n in expected.keys()|actual.keys() if expected.get(n)!=actual.get(n))
    if differences:
        raise ValueError('These assets differ from the base build: '+', '.join(differences[:6])+'. Use the same game/update/DLC/mods or rebuild the full base.')
    if parts == 'both' and 'fallback-shaders.bin' in record['content'] and 'fallback-shaders.bin' not in current:
        raise ValueError('The updated shader pack is missing its fallback shaders; regenerate the complete shader pack.')
    if parts == 'binary' and (contract or shader_contract()) != record['shader_contract']:
        raise ValueError('Shader code changed since the base build. Choose Binary + shaders instead of reusing incompatible base shaders.')
    return record


def update_build_info(now=None):
    now = now or datetime.now(timezone.utc)
    if now.tzinfo is None: raise ValueError('Build time must include a timezone')
    return UPDATE_VERSION, now.astimezone(timezone.utc)


@contextmanager
def title_output(destination, update_id):
    from win.objcache import file_locks
    from wup_safety import check_output
    check_output(destination)
    destination.parent.mkdir(parents=True,exist_ok=True)
    with file_locks(destination.parent/('.'+destination.name+'.lock')):
        if destination.exists():
            if destination.is_symlink() or any(p.is_symlink() for p in destination.rglob('*')):
                raise ValueError('Update output must not contain symbolic links')
            if set(p.name for p in destination.iterdir()) not in ({'code','content','meta'},{'code','content','meta','.3dii-base-links.json'}):
                raise ValueError('Existing update output has unrelated files')
            if int(ET.parse(destination/'code/app.xml').findtext('title_id'),16)!=update_id:
                raise ValueError('Existing update output belongs to another title')
        with tempfile.TemporaryDirectory(prefix='.wup-update-',dir=destination.parent) as temp:
            new, old = Path(temp)/'new', Path(temp)/'old'
            yield new
            existed=destination.exists()
            if existed: destination.replace(old)
            try: new.replace(destination)
            except BaseException:
                if existed: old.replace(destination)
                raise


def make_update(source, destination, parts='both', version=None, now=None, *, base_manifest, inputs=None, contract=None):
    if parts not in PARTS: raise ValueError('Choose binary or binary + shaders')
    now = now or datetime.now(timezone.utc)
    if version is None: version=UPDATE_VERSION
    if not 0 <= version <= 65535: raise ValueError('Wii U versions range from 0 to 65535')
    source, destination = Path(source), Path(destination)
    if destination.resolve()==source.resolve() or source.resolve() in destination.resolve().parents or destination.resolve() in source.resolve().parents:
        raise ValueError('Update and base title folders must be separate')
    app = ET.parse(source/'code/app.xml')
    base = int(app.findtext('title_id'),16)
    from wup_safety import check_title_id
    check_title_id(base)
    if base>>32 != 0x00050000: raise ValueError('An update needs a base game title')
    native_base_id(base)  # Reject legacy identities before creating any output.
    meta = ET.parse(source/'meta/meta.xml')
    if int(meta.findtext('title_id'),16)!=base: raise ValueError('Base metadata title ID mismatch')
    if not list((source/'code').glob('*.rpx')): raise ValueError('The updated executable is missing')
    if parts=='both' and not all((source/'content'/n).is_file() for n in ('shaders.gsh','shaders.idx')):
        raise ValueError('A shader update requires both shaders.gsh and shaders.idx')
    record = read_manifest(base_manifest, base)
    verify_base(source, record, parts, inputs=inputs, contract=contract)
    update = base | (0xe<<32)
    now = now or datetime.now(timezone.utc)
    with title_output(destination,update) as new:
        out=Path(new)
        for d in ('code','content','meta'): (out/d).mkdir(parents=True)
        for file in (source/'code').iterdir():
            if file.suffix in ('.rpx','.rpl') or file.name=='cos.xml': shutil.copy2(file,out/'code'/file.name)
        for tag,value in [('title_id',f'{update:016X}'),('title_version',f'{version:04X}'),('app_type',APP_TYPE)]:
            node=app.find(tag)
            if node is None: raise ValueError('Missing app.xml field: '+tag)
            node.text=value
        app.write(out/'code/app.xml',encoding='utf-8',xml_declaration=True)
        for file in (source/'meta').iterdir():
            if file.name!='meta.xml': shutil.copy2(file,out/'meta'/file.name)
        node=meta.find('title_version')
        if node is None: raise ValueError('Missing meta.xml title_version')
        node.text=str(version)  # meta.xml identifies the base game, not 0005000E.
        meta.write(out/'meta/meta.xml',encoding='utf-8',xml_declaration=True)
        if parts=='both':
            for name in ('shaders.gsh','shaders.idx','fallback-shaders.bin'):
                file=source/'content'/name
                if file.is_file(): shutil.copy2(file,out/'content'/name)
        # Options belong to this update, including explicit defaults that can
        # override an old base's presence-only no_menu flag or custom shortcut.
        for name in ('language.txt','menu_shortcut.txt'):
            file = source/'content'/name
            (out/'content'/name).write_bytes(file.read_bytes() if file.exists() else (b'-1\n' if name=='language.txt' else b'\n'))
        enabled = source/'content/menu_enabled.txt'
        enabled_value = enabled.read_bytes().strip() if enabled.exists() else (b'0' if (source/'content/no_menu').exists() else b'1')
        if enabled_value not in (b'0',b'1'):
            raise ValueError('Invalid menu enabled setting')
        (out/'content/menu_enabled.txt').write_bytes(enabled_value+b'\n')
        (out/'content'/UPDATE_MARKER).write_bytes((record['identity']+'\n').encode('ascii'))
        details=dict(base_title_id=f'{base:016X}',update_title_id=f'{update:016X}',version=version,
                     built_at=now.isoformat(),parts=parts,version_policy='fixed',base_identity=record['identity'])
        (out/'content/3dii-update.json').write_text(json.dumps(details,indent=2)+'\n')
        links={name:info['size'] for name,info in record['content'].items()
               if not (out/'content'/name).exists()}
        links[BASE_MARKER] = 65
        # PC-only input to the packer. These names become native FST link records,
        # not payload copies. Nintendo's update installer links them to the base.
        (out/'.3dii-base-links.json').write_text(json.dumps(dict(base_title_id=f'{base:016X}',files=links),indent=2)+'\n')
    return details
