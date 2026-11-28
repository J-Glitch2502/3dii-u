"""Include the requested update loader with its corresponding GPL source."""
from pathlib import Path
import hashlib
import json
import shutil
import zipfile


def bundle(app, module, source):
    app,module,source=map(Path,(app,module,source))
    if not module.is_file() or module.read_bytes()[:4]!=b'\x7fELF':
        raise ValueError('Supply the built RPXLoadingModule.wms')
    if not (source/'src/DevBundlePaths.h').is_file() or not (source/'LICENSE').is_file():
        raise ValueError('Supply the corresponding generic-development loader source with its licence')
    dest=app/'Aroma support';target=dest/'wiiu/environments/aroma/modules/RPXLoadingModule.wms'
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(module,target)
    shutil.copyfile(source/'LICENSE',dest/'LICENSE-GPL-3.0.txt')
    files=[p for p in source.rglob('*') if p.is_file() and not p.is_symlink()
           and (p.relative_to(source).parts[0] in ('src','tests') or p.relative_to(source).as_posix() in ('Makefile','README.md','LICENSE','Dockerfile','.clang-format'))
           and p.suffix not in ('.o','.elf','.wms','.pyc')]
    with zipfile.ZipFile(dest/'RPXLoadingModule-source.zip','w',zipfile.ZIP_DEFLATED) as z:
        for p in sorted(files):z.write(p,'RPXLoadingModule/'+p.relative_to(source).as_posix())
    (dest/'Build information.json').write_text(json.dumps(dict(module_sha256=hashlib.sha256(module.read_bytes()).hexdigest(),
        source_archive_sha256=hashlib.sha256((dest/'RPXLoadingModule-source.zip').read_bytes()).hexdigest(),
        upstream='https://github.com/wiiu-env/RPXLoadingModule',licence='GPL-3.0',source_files=len(files)),indent=2)+'\n')
    (dest/'Read me.txt').write_text('Aroma support for small Homebrew updates\n\n'
        'This is our RPXLoadingModule with generic .wuhb.dev updates. It is an Aroma module, not a .wps plugin.\n'
        'Copy wiiu/environments/aroma/modules/RPXLoadingModule.wms to that path on your SD card, replacing that module.\n'
        'If you use a differently named Aroma environment, put it in that environment\'s modules folder instead.\n'
        'Reload Aroma or reboot once. Keep the rest of your existing Aroma installation.\n'
        'See the main Homebrew import guide for building and importing updates.\n\n'
        'The corresponding project source and GPL-3.0 licence are included here. Follow its README and Makefile to build it.\n')
    return dest
