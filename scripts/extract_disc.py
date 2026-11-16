"""Extract XDVDFS without modifying the source; record hashes for verification."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def extract(image, destination, manifest):
    destination = destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    records = []
    directories = set()
    with image.open('rb') as disc:
        length = image.stat().st_size
        def read(offset, size):
            if offset < 0 or size < 0 or offset + size > length:
                raise ValueError('Disc extent outside image')
            disc.seek(offset)
            data = disc.read(size)
            if len(data) != size:
                raise ValueError('Truncated image')
            return data
        magic = b'MICROSOFT*XBOX*MEDIA'
        bases = [0, 0xFD90000, 0x2080000]
        base = next((b for b in bases if b + 0x10800 <= length and read(b + 0x10000, 20) == magic), None)
        if base is None:
            raise ValueError('XDVDFS volume descriptor not found')
        descriptor = read(base + 0x10000, 2048)
        if descriptor[-20:] != magic:
            raise ValueError('Invalid volume descriptor footer')
        root_sector, root_size = struct.unpack_from('<II', descriptor, 20)
        def directory(sector, size, parent):
            key = (sector, size)
            if key in directories:
                raise ValueError('Repeated directory extent')
            directories.add(key)
            if not size:
                return
            table = read(base + sector * 2048, size)
            pending, seen = [0], set()
            while pending:
                offset = pending.pop()
                if offset in seen or offset + 14 > size:
                    raise ValueError('Invalid directory tree')
                seen.add(offset)
                if table[offset:offset + 2] == b'\xff\xff':
                    if offset == 0:
                        continue  # XDVDFS empty directory sentinel
                    pending.append(offset + (2048 - offset % 2048))
                    continue
                left, right, start, count, attr, name_len = struct.unpack_from('<HHIIBB', table, offset)
                if offset + 14 + name_len > size:
                    raise ValueError('Invalid filename length')
                name = table[offset + 14:offset + 14 + name_len].decode('ascii')
                if not name or name in ('.', '..') or any(c in name for c in '/\\:'):
                    raise ValueError('Unsafe filename')
                relative = parent / name
                target = (destination / relative).resolve()
                if not target.is_relative_to(destination):
                    raise ValueError('Unsafe destination')
                if attr & 0x10:
                    target.mkdir(exist_ok=True)
                    directory(start, count, relative)
                else:
                    digest = hashlib.sha256()
                    existing = target.exists()
                    if existing and target.stat().st_size != count:
                        raise FileExistsError(target)
                    with target.open('rb' if existing else 'xb') as out:
                        position = base + start * 2048
                        remaining = count
                        while remaining:
                            chunk = read(position, min(remaining, 1024 * 1024))
                            if existing:
                                if out.read(len(chunk)) != chunk:
                                    raise ValueError(f'Existing file differs: {target}')
                            else:
                                out.write(chunk)
                            digest.update(chunk)
                            position += len(chunk)
                            remaining -= len(chunk)
                    records.append(dict(path=relative.as_posix(), size=count, sha256=digest.hexdigest()))
                pending.extend(p * 4 for p in (right, left) if p)
        directory(root_sector, root_size, Path())
    with image.open('rb') as source:
        source_hash = hashlib.file_digest(source, 'sha256').hexdigest()
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(dict(image=image.name, image_size=length, image_sha256=source_hash,
        partition_offset=base, files=sorted(records, key=lambda r: r['path'])), indent=2) + '\n')
    print(f'Extracted {len(records)} files, {sum(r["size"] for r in records):,} bytes to {destination}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--manifest', type=Path, default=Path('analysis/disc_manifest.json'))
    args = parser.parse_args()
    extract(args.image, args.destination, args.manifest)
