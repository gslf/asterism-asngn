#!/usr/bin/env python3
"""Verify the pinned component combination; emit the exact engine revision used."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def verify(root, manifest, allow_engine_dirty=False):
    resolved = {'schema': 1, 'components': {}}
    for component, spec in manifest['components'].items():
        path = root / f'asterism-{component}'
        revision = git(path, 'rev-parse', 'HEAD')
        if spec['revision'] != 'checkout' and revision != spec['revision']:
            raise ValueError(f'{component}: expected {spec["revision"]}, found {revision}')
        dirty = git(path, 'status', '--porcelain', '--untracked-files=normal', '--ignore-submodules=all')
        if dirty and not (component == 'asngn' and allow_engine_dirty):
            raise ValueError(f'{component}: dirty source cannot represent a pinned release')
        for header, expected in spec.get('headers', {}).items():
            actual = hashlib.sha256((path / header).read_bytes()).hexdigest()
            if actual != expected:
                raise ValueError(f'{component}: public contract changed: {header}')
        resolved['components'][component] = {'revision': revision, 'dirty': bool(dirty)}
    for component in ('asper', 'astools'):
        path = root / f'asterism-{component}'
        entry = git(path, 'ls-tree', 'HEAD', 'deps/xcdn-c').split()
        if len(entry) < 3 or entry[2] != manifest['xcdn_revision']:
            raise ValueError(f'{component}: xCDN gitlink differs from the shared pin')
    llama = git(root / 'asterism-asper', 'ls-tree', 'HEAD', 'deps/llama.cpp').split()
    if len(llama) < 3 or llama[2] != manifest['llama_revision']:
        raise ValueError('asper: llama.cpp gitlink differs from the release pin')
    resolved['llama_revision'] = manifest['llama_revision']
    resolved['xcdn_revision'] = manifest['xcdn_revision']
    return resolved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--manifest', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'release.json')
    parser.add_argument('--allow-engine-dirty', action='store_true')
    args = parser.parse_args()
    try:
        result = verify(args.root, json.loads(args.manifest.read_text()), args.allow_engine_dirty)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Release verification failed: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
