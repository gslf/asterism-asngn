#!/usr/bin/env python3
"""Verify the pinned component combination; emit the exact engine revision used."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def submodule(path, revision, required):
    if not (path / '.git').exists():
        if required:
            raise ValueError(f'{path}: required submodule is not initialized')
        return {'initialized': False, 'revision': revision}
    actual = git(path, 'rev-parse', 'HEAD')
    dirty = git(path, 'status', '--porcelain', '--untracked-files=normal', '--ignore-submodules=none')
    if actual != revision or dirty:
        raise ValueError(f'{path}: submodule checkout differs from its clean pin')
    return {'initialized': True, 'revision': actual}


def verify(root, manifest, allow_engine_dirty=False, with_llama=False):
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
            actual = hashlib.sha256((path / header).read_bytes().replace(b'\r\n', b'\n')).hexdigest()
            if actual != expected:
                raise ValueError(f'{component}: public contract changed: {header}')
        resolved['components'][component] = {'revision': revision, 'dirty': bool(dirty)}
    dependency = json.loads((root / 'asterism-asper/dependencies.json').read_text())
    model = manifest['components']['asmodel']
    expected = {'schema': 1, 'asmodel': {key: model[key] for key in ('repository', 'revision')}}
    if not isinstance(dependency, dict) or type(dependency.get('schema')) is not int or dependency != expected:
        raise ValueError('asper: standalone dependency pin differs from the coordinated release')
    resolved['submodules'] = {}
    for component in ('asper', 'astools'):
        path = root / f'asterism-{component}'
        entry = git(path, 'ls-tree', 'HEAD', 'deps/xcdn-c').split()
        if len(entry) < 3 or entry[2] != manifest['xcdn_revision']:
            raise ValueError(f'{component}: xCDN gitlink differs from the shared pin')
        resolved['submodules'][f'{component}/xcdn'] = submodule(
            path / 'deps/xcdn-c', manifest['xcdn_revision'], True)
    llama = git(root / 'asterism-asper', 'ls-tree', 'HEAD', 'deps/llama.cpp').split()
    if len(llama) < 3 or llama[2] != manifest['llama_revision']:
        raise ValueError('asper: llama.cpp gitlink differs from the release pin')
    resolved['submodules']['asper/llama'] = submodule(
        root / 'asterism-asper/deps/llama.cpp', manifest['llama_revision'], with_llama)
    resolved['llama_revision'] = manifest['llama_revision']
    resolved['xcdn_revision'] = manifest['xcdn_revision']
    return resolved


def refresh(root, manifest, with_llama=False):
    """Prepare pins from clean sibling commits, retaining submodule admission."""
    updated = json.loads(json.dumps(manifest))
    for component, spec in updated['components'].items():
        path = root / f'asterism-{component}'
        spec['revision'] = 'checkout' if component == 'asngn' else git(path, 'rev-parse', 'HEAD')
        for header in spec.get('headers', {}):
            spec['headers'][header] = hashlib.sha256(
                (path / header).read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    verify(root, updated, allow_engine_dirty=True, with_llama=with_llama)
    return updated


def refresh_asper_dependency(root):
    """Update Asper only after asmodel has a clean, concrete commit."""
    model = root / 'asterism-asmodel'
    if git(model, 'status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('asmodel: commit its changes before updating the Asper pin')
    return {'schema': 1, 'asmodel': {'repository': 'gslf/asterism-asmodel',
                                    'revision': git(model, 'rev-parse', 'HEAD')}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--manifest', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'release.json')
    parser.add_argument('--allow-engine-dirty', action='store_true')
    update = parser.add_mutually_exclusive_group()
    update.add_argument('--update-asper-pin', action='store_true',
                        help='write Asper dependency pin after committing asmodel')
    update.add_argument('--update-pins', action='store_true',
                        help='write engine pins after committing all siblings')
    parser.add_argument('--with-llama', action='store_true',
                        help='require the pinned native backend checkout, not only its gitlink')
    args = parser.parse_args()
    try:
        if args.update_asper_pin:
            result = refresh_asper_dependency(args.root)
            destination = args.root / 'asterism-asper/dependencies.json'
            destination.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
        elif args.update_pins:
            result = refresh(args.root, json.loads(args.manifest.read_text()), args.with_llama)
            args.manifest.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
        else:
            result = verify(args.root, json.loads(args.manifest.read_text()), args.allow_engine_dirty, args.with_llama)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Release verification failed: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
