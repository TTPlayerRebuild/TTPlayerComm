"""Synchronize/check the optional audio-format SDK in sibling repositories."""
import argparse
from pathlib import Path

PLUGINS = 'aac ogg flac ape webm ac3dts mod mpc asf rm tak vqf dts dsd'.split()
FILES = ('audio_format_probe.h', 'audio_format_probe_impl.h')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    sdk = Path(__file__).resolve().parents[1] / 'sdk/include/ttpcomm'
    roots = [args.workspace / ('ttp_' + p) / 'include/ttpcomm' for p in PLUGINS]
    roots += [args.workspace / 'rebuild/third_party/ttpcomm-sdk/include/ttpcomm']
    mismatches = []
    for root in roots:
        # A standalone clone must not create empty sibling repositories.
        if not (root.parents[1] if root.parent.name == 'include' else root.parents[3]).exists():
            continue
        for name in FILES:
            expected = (sdk / name).read_bytes()
            target = root / name
            if not target.exists() or target.read_bytes() != expected:
                if args.check:
                    mismatches.append(str(target))
                else:
                    root.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(expected)
    if mismatches:
        raise SystemExit('SDK copies differ:\n' + '\n'.join(mismatches))
    print('Audio format SDK copies verified' if args.check else 'Audio format SDK copies synchronized')

if __name__ == '__main__':
    main()
