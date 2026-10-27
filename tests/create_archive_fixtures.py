#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ZIP metadata fixtures for the native preview integration test."""
from pathlib import Path
import stat
import struct
import sys
import zipfile


def create(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(directory / 'archive.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('Documents/', b'')
        archive.writestr('Documents/README.md', '# Inside the archive\n')
        archive.writestr('Pictures/', b'')
        archive.writestr('Pictures/sample.svg', '<svg xmlns="http://www.w3.org/2000/svg"/>')
        archive.writestr('.hidden.txt', 'Hidden entries are included in archive listings.\n')
    with zipfile.ZipFile(directory / 'large.zip', 'w') as archive:
        for index in range(645):
            archive.writestr(f'folder-{index // 10}/item-{index:04}.txt', str(index))
    with zipfile.ZipFile(directory / 'empty.zip', 'w'):
        pass
    with zipfile.ZipFile(directory / 'paths.zip', 'w') as archive:
        archive.writestr('../outside.txt', 'Never extract this')
        archive.writestr(str(directory / 'absolute-outside.txt'), 'Never extract this either')
        archive.writestr('folder/new\nline.txt', 'Literal path')
        archive.writestr('folder/' + 'a' * 1500 + '.txt', 'Long path')
        link = zipfile.ZipInfo('shortcut')
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        archive.writestr(link, '../outside.txt')
    payload = directory / 'unread-payload.zip'
    with zipfile.ZipFile(payload, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('contents.txt', 'Regular entry data' * 100)
    data = bytearray(payload.read_bytes())
    start = 30 + struct.unpack_from('<H', data, 26)[0] + struct.unpack_from('<H', data, 28)[0]
    data[start] ^= 0xff  # Keep directory metadata valid, corrupt only compressed member data.
    payload.write_bytes(data)
    (directory / 'broken.zip').write_bytes(b'This is not a ZIP archive')
    data = bytearray((directory / 'empty.zip').read_bytes())
    struct.pack_into('<I', data, 12, 16 * 1024 * 1024 + 1)
    (directory / 'oversized-index.zip').write_bytes(data)
    record = struct.pack('<4sQHHIIQQQQ', b'PK\x06\x06', 44, 45, 45, 0, 0, 0, 0, 0, 0)
    locator = struct.pack('<4sIQI', b'PK\x06\x07', 0, 0, 1)
    end = struct.pack('<4sHHHHIIH', b'PK\x05\x06', 0, 0, 0xffff, 0xffff, 0xffffffff, 0xffffffff, 0)
    (directory / 'zip64-empty.zip').write_bytes(record + locator + end)
    huge = struct.pack('<4sQHHIIQQQQ', b'PK\x06\x06', 44, 45, 45, 0, 0, 100001, 100001, 0, 0)
    (directory / 'too-many-entries.zip').write_bytes(huge + locator + end)


if __name__ == '__main__':
    create(sys.argv[1])
