#!/usr/bin/env python3
"""Publish a WUHB companion RPX/content release without touching the bundle or saves."""
import argparse
from ftplib import FTP, all_errors, error_perm
from hashlib import sha256
from io import BytesIO
from pathlib import Path, PurePosixPath
import re
import uuid

SD = '/fs/vol/external01/'


def validate_bundle(bundle):
    parts = bundle.split('/')
    if (not bundle.startswith('wiiu/apps/') or not bundle.endswith('.wuhb') or
            any(p in ('', '.', '..') for p in parts) or
            any(ord(c) < 32 or ord(c) == 127 or c in ':\\' for c in bundle)):
        raise ValueError('Expected an SD-relative wiiu/apps/.../Name.wuhb path')


def rpx_header(path):
    with path.open('rb') as f:
        h = f.read(52)
    if len(h) != 52 or h[:9] != b'\x7fELF\x01\x02\x01\xca\xfe' or h[16:20] != b'\xfe\x01\x00\x14':
        raise ValueError('Not a Wii U RPX executable')


def remote_hash(ftp, path):
    digest = sha256()
    ftp.retrbinary('RETR ' + path, digest.update, blocksize=128 << 10)
    return digest.digest()


def mkdirs(ftp, path):
    current = ''
    for part in PurePosixPath(path).parts[1:]:
        current += '/' + part
        try:
            ftp.mkd(current)
        except error_perm:
            # Do not mistake permission failures for an already-existing directory.
            previous = ftp.pwd()
            ftp.cwd(current)
            ftp.cwd(previous)


def publish(ftp, bundle, rpx, content, release, *, readback=True):
    validate_bundle(bundle)
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', release):
        raise ValueError('Release ID must contain 1–64 letters, numbers, underscores or hyphens')
    rpx_header(rpx)
    if content is not None and (not content.is_dir() or content.is_symlink()):
        raise ValueError('Content must be a directory')
    files = [(rpx, 'root.rpx')]
    if content:
        for p in sorted(content.rglob('*')):
            if p.is_symlink():
                raise ValueError('Content symlinks are not allowed')
            if p.is_file():
                rel = p.relative_to(content).as_posix()
                if any(ord(c) < 32 or ord(c) == 127 or c in ':\\' for c in rel):
                    raise ValueError('Invalid content filename')
                files.append((p, 'content/' + rel))
        shader_files = [content / n for n in ('shaders.gsh', 'shaders.idx')]
        if any(p.exists() for p in shader_files) and not all(p.is_file() for p in shader_files):
            raise ValueError('Supply shaders.gsh and shaders.idx together')
    ftp.voidcmd('TYPE I')
    if not ftp.size(SD + bundle):
        raise ValueError('Base WUHB is missing or empty on the SD card')
    companion = SD + bundle + '.dev'
    builds = companion + '/builds'
    mkdirs(ftp, builds)
    destination = builds + '/' + release
    # Releases are immutable. An interrupted attempt can be removed separately,
    # or retried under a new ID; never overwrite a running title's shader files.
    ftp.mkd(destination)
    total = sum(p.stat().st_size for p, _ in files)
    sent = 0
    for p, relative in files:
        remote = destination + '/' + relative
        mkdirs(ftp, str(PurePosixPath(remote).parent))
        before = p.stat()
        with p.open('rb') as stream:
            expected = sha256(stream.read()).digest()
            stream.seek(0)
            ftp.storbinary('STOR ' + remote, stream, blocksize=128 << 10)
        if p.stat().st_mtime_ns != before.st_mtime_ns:
            raise ValueError('Source changed during upload: ' + relative)
        if readback:
            if remote_hash(ftp, remote) != expected:
                raise ValueError('Readback verification failed: ' + relative)
        elif ftp.size(remote) != before.st_size:
            raise ValueError('Uploaded file size differs: ' + relative)
        sent += before.st_size
        action = 'Verified' if readback else 'Uploaded'
        print(f'{action} {relative}: {sent}/{total} bytes', flush=True)
    pending = companion + '/active.' + uuid.uuid4().hex + '.upload'
    marker = (release + '\n').encode('ascii')
    ftp.storbinary('STOR ' + pending, BytesIO(marker))
    if readback:
        if remote_hash(ftp, pending) != sha256(marker).digest():
            raise ValueError('Activation marker readback failed')
    elif ftp.size(pending) != len(marker):
        raise ValueError('Incomplete activation marker upload')
    active = companion + '/active.txt'
    backup = companion + '/active.' + uuid.uuid4().hex + '.bak'
    had_active = False
    try:
        ftp.size(active)
        had_active = True
    except error_perm as error:
        # FTPiiU uses 553 for a missing marker on a first-time title.
        missing = str(error).startswith('550') or str(error).lower().startswith('553 no such file or directory')
        if not missing:
            raise
    if had_active:
        ftp.rename(active, backup)
    try:
        ftp.rename(pending, active)
    except all_errors:
        if had_active:
            ftp.rename(backup, active)
        raise
    print(f'Published {release} for {bundle}; {total} bytes. Base WUHB and saves unchanged.', flush=True)
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('host')
    ap.add_argument('bundle', help='SD-relative path, e.g. wiiu/apps/Kirby Triple Deluxe.wuhb')
    ap.add_argument('rpx', type=Path)
    ap.add_argument('--content', type=Path, help='Only changed content files; absent files use the original WUHB')
    ap.add_argument('--release', required=True)
    ap.add_argument('--port', type=int, default=21)
    ap.add_argument('--no-readback', action='store_true', help='Skip downloading uploaded files again; check remote sizes only')
    args = ap.parse_args()
    try:
        with FTP(timeout=30) as ftp:
            ftp.connect(args.host, args.port)
            ftp.login()
            publish(ftp, args.bundle, args.rpx, args.content, args.release, readback=not args.no_readback)
    except (*all_errors, OSError, ValueError) as error:
        ap.exit(1, f'Publish failed: {error}\n')


if __name__ == '__main__':
    main()
