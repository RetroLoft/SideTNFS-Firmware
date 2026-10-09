"""Generate redistributable GLOAD data; no game files or downloads required."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib

ROOT = Path(__file__).resolve().parent
SIZES = (3678, 13448, 61104, 77878, 56371, 224092)


def fixtures():
    # Preserve the original generic-v1 seed so existing fixtures stay compatible.
    # Unique SHA-256 counter blocks expose repeated, reordered or stale reads.
    for index, size in enumerate(SIZES, 1):
        name = ('FILES/' if index > 4 else '') + f'FILE{index:02}.BIN'
        data = b''.join(hashlib.sha256(
            f'RLOAD generic-v1:{index}:{block}'.encode('ascii')).digest()
            for block in range((size + 31) // 32))[:size]
        yield name, data


def metadata():
    return [dict(path=name.replace('/', '\\'), size=len(data),
                 crc32=zlib.crc32(data), sha256=hashlib.sha256(data).hexdigest())
            for name, data in fixtures()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', type=Path, default=ROOT / 'fixtures')
    args = parser.parse_args()
    expected = json.loads((ROOT / 'assets.json').read_text())
    if metadata() != expected:
        raise SystemExit('Fixture generator differs from checked-in assets.json')
    data_files = list(fixtures())
    for name, data in data_files:
        target = args.directory / name
        if target.exists() and target.read_bytes() != data:
            raise SystemExit(f'Refusing to overwrite different data: {target}')
    for name, data in data_files:
        target = args.directory / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    print(f'Generated 6 files, {sum(SIZES)} bytes in {args.directory}')


if __name__ == '__main__':
    main()
