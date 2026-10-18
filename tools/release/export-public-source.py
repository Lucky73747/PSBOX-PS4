#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create a checked, history-free local source directory. Never publishes."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from public_tree import selected_files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument('--output', type=Path)
    destination.add_argument('--refresh-manifest', action='store_true',
                             help='Regenerate the checkout manifest after all source checks')
    args = parser.parse_args()
    root = args.root.resolve()
    if args.refresh_manifest:
        spec = importlib.util.spec_from_file_location('public_check', Path(__file__).with_name('check-public-tree.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        errors, _ = module.check(root, verify_manifest=False)
        if errors:
            raise SystemExit('Source checks failed:\n' + '\n'.join(errors))
        entries = []
        for rel in selected_files(root):
            if str(rel) == 'SOURCE-MANIFEST.json':
                continue
            data = (root / rel).read_bytes()
            entries.append({'path': str(rel), 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        # File hashes identify the snapshot without exposing checkout ancestry.
        base = None
        manifest = {'format': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
                    'application': 'PSBOX-PS4', 'scope': 'current selected source, including uncommitted changes',
                    'history_included': False, 'base_commit_not_snapshot_commit': base, 'files': entries}
        target = root / 'SOURCE-MANIFEST.json'
        pending = root / 'SOURCE-MANIFEST.pending'
        pending.write_text(json.dumps(manifest, indent=2)+'\n')
        pending.replace(target)
        errors, count = module.check(root)
        if errors:
            raise SystemExit('Manifest validation failed:\n' + '\n'.join(errors))
        print(f'Refreshed manifest: {count} selected files. Nothing was published.')
        return
    dest = args.output.absolute()
    if dest.exists() or dest.is_symlink():
        parser.error('Output must not already exist; existing work is never overwritten')
    resolved_dest = dest.resolve()
    if resolved_dest.is_relative_to(root) or root.is_relative_to(resolved_dest):
        parser.error('Choose an output directory outside the source checkout')
    spec = importlib.util.spec_from_file_location('public_check', Path(__file__).with_name('check-public-tree.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    errors, _ = module.check(root)
    if errors:
        raise SystemExit('Source checks failed:\n' + '\n'.join(errors))
    dest.parent.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix='.'+dest.name+'-pending-', dir=dest.parent))
    try:
        entries = []
        for rel in selected_files(root):
            if str(rel) == 'SOURCE-MANIFEST.json':
                continue
            p = root / rel
            if p.is_symlink() or any(parent.is_symlink() for parent in p.parents if parent != root):
                raise RuntimeError(f'Refusing symlink: {rel}')
            before = p.stat()
            data = p.read_bytes()
            after = p.stat()
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise RuntimeError(f'Source changed during export: {rel}; retry after editing stops')
            q = temp / rel
            q.parent.mkdir(parents=True, exist_ok=True)
            q.write_bytes(data)
            q.chmod(0o755 if before.st_mode & 0o111 else 0o644)
            entries.append({'path': str(rel), 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        base = None
        # File hashes identify the snapshot without exposing checkout ancestry.
        manifest = {'format': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
                    'application': 'PSBOX-PS4', 'scope': 'current selected source, including uncommitted changes',
                    'history_included': False, 'base_commit_not_snapshot_commit': base, 'files': entries}
        (temp / 'SOURCE-MANIFEST.json').write_text(json.dumps(manifest, indent=2)+'\n')
        errors, count = module.check(temp)
        if errors:
            raise RuntimeError('Export checks failed:\n'+'\n'.join(errors))
        temp.rename(dest)
        print(f'Prepared local source export: {dest} ({count} files). Nothing was published.')
    except BaseException:
        shutil.rmtree(temp)
        raise

if __name__ == '__main__':
    main()
