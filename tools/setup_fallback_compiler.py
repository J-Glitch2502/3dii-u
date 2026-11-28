#!/usr/bin/env python3
"""Install the pinned public CafeGLSL fallback compiler, separate from v0.2.0."""
import argparse
import hashlib
import io
import os
from pathlib import Path
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '1.0.0-rc1'
PACKAGES = {
    'linux-x86_64': ('tar.xz','fdbd561539080f1db74ec3f248edee4e1988d329a4d20f2273c72adf55015f97'),
    'windows-x86_64': ('zip','416e07d04df92ffe32bd8bfe9e8fa556a9202c1c6a282bd26e249b1fe96e410a'),
}


def ensure(platform=None, archive=None):
    platform = platform or ('windows-x86_64' if os.name == 'nt' else 'linux-x86_64')
    suffix,digest=PACKAGES[platform]
    target=ROOT/'third_party/cafeglsl_fallback'/platform
    executable=target/'bin'/('glslcompiler.exe' if platform.startswith('windows') else 'glslcompiler')
    if executable.is_file():return executable
    name=f'cafeglsl-{VERSION}-{platform}'
    if archive:data=Path(archive).read_bytes()
    else:
        url=f'https://github.com/Exzap/CafeGLSL/releases/download/v{VERSION}/{name}.{suffix}'
        with urllib.request.urlopen(url,timeout=60) as response:data=response.read(64<<20)
    if hashlib.sha256(data).hexdigest()!=digest:raise ValueError('CafeGLSL archive differs from the pinned release')
    if suffix=='zip':
        with zipfile.ZipFile(io.BytesIO(data)) as bundle:
            files={p:bundle.read(p) for p in bundle.namelist() if not p.endswith('/')}
    else:
        with tarfile.open(fileobj=io.BytesIO(data),mode='r:xz') as bundle:
            files={p.name:bundle.extractfile(p).read() for p in bundle.getmembers() if p.isfile()}
    for path,contents in files.items():
        relative=Path(path).relative_to(name)
        if any(part in ('..','') for part in relative.parts) or relative.is_absolute():raise ValueError('Unsafe compiler archive member')
        destination=target/relative;destination.parent.mkdir(parents=True,exist_ok=True)
        temporary=destination.with_suffix(destination.suffix+'.part');temporary.write_bytes(contents);temporary.replace(destination)
    if not executable.is_file():raise ValueError('Pinned release is missing its compiler')
    executable.chmod(executable.stat().st_mode|0o111)
    print('Fallback compiler:',executable,flush=True)
    return executable


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--platform',choices=PACKAGES);p.add_argument('--archive',type=Path)
    a=p.parse_args();ensure(a.platform,a.archive)
