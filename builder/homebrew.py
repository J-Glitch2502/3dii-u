"""Homebrew bundles and small executable/shader exports for the Builder."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import time
import zlib

PARTS = ('full', 'binary', 'shaders', 'both')
MANIFEST = '3dii-homebrew.json'


def filename(name):
    value = re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', name).strip(' .') or '3Dii U Game'
    if value.split('.')[0].upper() in {'CON','PRN','AUX','NUL',*[f'COM{i}' for i in range(1,10)],*[f'LPT{i}' for i in range(1,10)]}:
        value = '_' + value
    value = value[:120].rstrip(' .')
    while len(value.encode('utf-8')) > 180: value = value[:-1]
    return value


def copy_digest(source, destination):
    h = hashlib.sha256()
    with Path(source).open('rb') as src, Path(destination).open('wb') as dst:
        while data := src.read(4 << 20):
            dst.write(data);h.update(data)
    return h.hexdigest()


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        while data := f.read(4 << 20): h.update(data)
    return h.hexdigest()


def identity(game, program_id, name, root):
    game, root = Path(game), Path(root)
    revision = re.search(r'kShaderGenRevision\s*=\s*(\d+)', (root/'port/wiiu/shadergen.h').read_text())
    if not revision: raise ValueError('The shader compatibility revision is missing')
    return dict(version=1, name=name, program_id=f'{program_id:016x}',
                game_code_sha256=digest(game/'code.bin'), romfs_sha256=digest(game/'romfs.bin'),
                shader_revision=int(revision[1]))


def tga_png(source, destination):
    """Title artwork is uncompressed BGR(A) TGA; WUHB takes PNG, without an extra image dependency."""
    data = Path(source).read_bytes()
    if len(data)<18: raise ValueError('Truncated title artwork')
    ident,cmap,kind,_,_,_,_,_,w,h,bits,flags=struct.unpack_from('<BBBHHBHHHHBB',data)
    channels=bits//8
    if cmap or kind!=2 or channels not in (3,4) or not (0<w<=2048 and 0<h<=2048):
        raise ValueError('Unsupported title artwork')
    pixels=data[18+ident:]
    if len(pixels)<w*h*channels: raise ValueError('Truncated title pixels')
    rows=[]
    for y in range(h):
        sy=y if flags&32 else h-1-y
        row=bytearray()
        for x in range(w):
            sx=w-1-x if flags&16 else x; at=(sy*w+sx)*channels
            b,g,r=pixels[at:at+3];row.extend((r,g,b))
            if channels==4:row.append(pixels[at+3])
        rows.append(b'\0'+row)
    def chunk(tag,payload):
        return struct.pack('>I',len(payload))+tag+payload+struct.pack('>I',zlib.crc32(tag+payload)&0xffffffff)
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6 if channels==4 else 2,0,0,0))
    Path(destination).write_bytes(png+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b''))


def bundle(rpx, title, output, name, tool, manifest, guide):
    """Write the WUHB atomically, leaving the original selected until packing succeeds."""
    rpx,title,output,tool=map(Path,(rpx,title,output,tool))
    if not tool.is_file():raise ValueError('wuhbtool is missing from the bundled devkitPro tools')
    output.parent.mkdir(parents=True,exist_ok=True)
    (title/'content'/MANIFEST).write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    images=[]
    for stem,option in [('iconTex','icon'),('bootTvTex','tv-image'),('bootDrcTex','drc-image')]:
        png=title/(stem+'.png');tga_png(title/'meta'/(stem+'.tga'),png);images.append(f'--{option}={png}')
    temporary=output.with_name(output.name+'.'+os.urandom(12).hex()+'.new')
    marker=Path(str(output)+'.dev')/'active.txt'
    disabled=marker.with_name('active.disabled-by-full-build.'+os.urandom(12).hex())
    moved=False
    try:
        args=[str(tool),str(rpx),str(temporary),f'--content={title/"content"}',f'--name={name}',f'--short-name={name}', '--author=3Dii U',*images]
        subprocess.run(args,check=True)
        with temporary.open('rb') as f:header=f.read(4)
        if header!=b'WUHB':raise ValueError('wuhbtool did not produce a WUHB')
        if temporary.stat().st_size>0xffffffff:raise ValueError('This WUHB exceeds the FAT32 single-file limit')
        # An old companion must not keep overriding a newly rebuilt full bundle.
        if marker.is_file():os.replace(marker,disabled);moved=True
        try:os.replace(temporary,output)
        except BaseException:
            if moved:os.replace(disabled,marker)
            raise
        output.with_suffix('.wuhb.build.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
        shutil.copyfile(guide,output.parent/'Homebrew import guide.txt')
        return output
    finally:
        temporary.unlink(missing_ok=True)


def export_update(rpx, shaders, output, name, parts, manifest, guide):
    if parts not in ('binary','shaders','both'):raise ValueError('Choose binary, shaders, or both')
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    folder=output/(filename(name)+'-update-'+time.strftime('%Y%m%d-%H%M%S')+'-'+os.urandom(12).hex()[:6])
    incoming=folder.with_name('.'+folder.name+'.new');incoming.mkdir()
    try:
        files={}
        if parts in ('binary','both'):
            dest=incoming/'root.rpx';files['root.rpx']=copy_digest(rpx,dest)
        if parts in ('shaders','both'):
            content=incoming/'content';content.mkdir()
            for n in ('shaders.gsh','shaders.idx'):
                src=Path(shaders)/n
                if not src.is_file():raise ValueError('Both shader files are required')
                files['content/'+n]=copy_digest(src,content/n)
        meta=dict(manifest,parts=parts,files=files)
        (incoming/'update.json').write_text(json.dumps(meta,indent=2)+'\n',encoding='utf-8')
        shutil.copyfile(guide,incoming/'Homebrew import guide.txt')
        os.replace(incoming,folder)
        return folder
    finally:
        if incoming.exists():shutil.rmtree(incoming)
