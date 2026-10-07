"""Export/check a pinned SDK snapshot for independently buildable consumers."""
import argparse
import hashlib
import json
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    files = {str(p.relative_to(root / 'sdk')).replace('\\', '/'): p.read_bytes()
             for p in (root / 'sdk').rglob('*') if p.is_file()}
    files['include/ttpcomm_api.h'] = (root/'include/ttpcomm_api.h').read_bytes()
    files['include/ttpcomm_objects.h'] = (root/'include/ttpcomm_objects.h').read_bytes()
    files['src/ttpcomm_api.c'] = (root/'src/ttpcomm_api.c').read_bytes().replace(
        b'"../include/ttpcomm_api.h"', b'"ttpcomm_api.h"')
    files['SDK_MANIFEST.json'] = (json.dumps(dict(schema=1, source='TTPlayerComm',
        files={name: hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()
               for name, data in sorted(files.items())}), indent=2)+'\n').encode()
    for name, data in files.items():
        target = args.destination / name
        if args.check:
            if not target.exists() or target.read_bytes().replace(b'\r\n', b'\n') != data.replace(b'\r\n', b'\n'):
                raise SystemExit('Stale SDK snapshot: ' + name)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    print(f'SDK {"verified" if args.check else "exported"}: {len(files)} files')

if __name__ == '__main__':
    main()
