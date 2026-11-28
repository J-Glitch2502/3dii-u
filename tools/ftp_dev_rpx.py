#!/usr/bin/env python3
"""Replace the executable of an installed 3Dii U dev title over FTPiiU."""

import argparse
from ftplib import FTP, all_errors
import hashlib
from io import BytesIO
from pathlib import Path
import re
from uuid import uuid4
import xml.etree.ElementTree as ET


def remote_digest(ftp, path):
    digest = hashlib.sha256()
    ftp.retrbinary('RETR ' + path, digest.update)
    return digest.digest()


def local_digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').digest()


def title_id(xml):
    value = ET.fromstring(xml).findtext('title_id', '').strip().lower()
    if not re.fullmatch(r'00050000[0-9a-f]{8}', value):
        raise ValueError('Expected an installed Wii U application title ID')
    return value


def upload(ftp, title, rpx, volume):
    local_id = title_id((title / 'code/app.xml').read_bytes())
    if not re.fullmatch(r'g_[a-z0-9]+(?:_v\d+)?\.rpx', rpx.name) or not (title / 'code' / rpx.name).is_file():
        raise ValueError('The rebuilt RPX must match the name in the base title')
    with rpx.open('rb') as stream:
        if stream.read(4) != b'\x7fELF':
            raise ValueError('The rebuilt file is missing the RPX executable header')
    root = f'/{volume}/usr/title/{local_id[:8]}/{local_id[8:]}'
    metadata = BytesIO()
    ftp.retrbinary('RETR ' + root + '/code/app.xml', metadata.write)
    if title_id(metadata.getvalue()) != local_id:
        raise ValueError('The remote title ID does not match the base title')
    for asset in ('shaders.idx', 'shaders.gsh'):
        if remote_digest(ftp, root + '/content/' + asset) != local_digest(title / 'content' / asset):
            raise ValueError(f'The installed {asset} does not match the base title')

    ftp.cwd(root + '/code')
    suffix = uuid4().hex[:12]
    pending = f'{rpx.name}.{suffix}.upload'
    backup = f'{rpx.name}.{suffix}.bak'
    try:
        with rpx.open('rb') as stream:
            ftp.storbinary('STOR ' + pending, stream)
        if remote_digest(ftp, pending) != local_digest(rpx):
            raise ValueError('RPX readback failed verification')
        ftp.rename(rpx.name, backup)
        try:
            ftp.rename(pending, rpx.name)
        except all_errors:
            try:
                ftp.rename(backup, rpx.name)
            except all_errors as error:
                raise ValueError(f'Could not restore the prior RPX. Check {root}/code/{backup}: {error}') from error
            raise
    except (*all_errors, ValueError):
        try:
            ftp.delete(pending)
        except all_errors:
            pass
        raise
    print(f'Updated {root}/code/{rpx.name}. Previous RPX saved as {backup}.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('base_title', type=Path, help='unpacked build matching the installed dev title')
    parser.add_argument('rpx', type=Path, help='code-only rebuild from that same prepared game')
    parser.add_argument('--port', type=int, default=21)
    parser.add_argument('--volume', choices=('storage_usb', 'storage_mlc'), required=True)
    args = parser.parse_args()
    try:
        with FTP(timeout=20) as ftp:
            ftp.connect(args.host, args.port)
            ftp.login()
            upload(ftp, args.base_title, args.rpx, args.volume)
    except (*all_errors, ValueError, ET.ParseError) as error:
        parser.exit(1, f'RPX update failed: {error}\n')


if __name__ == '__main__':
    main()
