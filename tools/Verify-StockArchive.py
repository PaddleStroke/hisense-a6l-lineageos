"""Validate and inventory a file-level stock archive without extracting it.

This does not establish partition-backup completeness or restorability.
"""
import argparse
import csv
import hashlib
import json
import pathlib
import tarfile


def verify(archive: pathlib.Path):
    archive = archive.resolve(strict=True)
    output = archive.with_suffix('.inventory.csv')
    summary_path = archive.with_suffix('.verification.json')
    file_count = 0
    data_bytes = 0
    member_count = 0
    with output.open('w', newline='', encoding='utf-8') as inventory:
        writer = csv.writer(inventory)
        writer.writerow(['path', 'type', 'bytes', 'sha256', 'link_target'])
        with tarfile.open(archive, 'r|') as tar:
            for member in tar:
                member_count += 1
                digest = ''
                if member.isfile():
                    hasher = hashlib.sha256()
                    size = 0
                    with tar.extractfile(member) as content:
                        while chunk := content.read(4 * 1024 * 1024):
                            hasher.update(chunk)
                            size += len(chunk)
                    if size != member.size:
                        raise ValueError(f'Truncated member: {member.name}')
                    digest = hasher.hexdigest()
                    data_bytes += size
                    file_count += 1
                writer.writerow([member.name, member.type.decode('ascii'),
                                 member.size, digest, member.linkname])
    archive_size = archive.stat().st_size
    with archive.open('rb') as source:
        if archive_size < 1024 or archive_size % 512:
            raise ValueError('Missing complete tar blocks')
        source.seek(-1024, 2)
        if source.read() != bytes(1024):
            raise ValueError('Missing tar end markers')
        source.seek(0)
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    summary = dict(archive=archive.name, archive_bytes=archive_size,
                   archive_sha256=digest, members=member_count,
                   regular_files=file_count, regular_file_bytes=data_bytes,
                   structurally_valid=True, raw_partition_backup=False,
                   completeness='Partial file-level copy; review capture stderr and exit status')
    summary_path.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=pathlib.Path)
    verify(parser.parse_args().archive)
