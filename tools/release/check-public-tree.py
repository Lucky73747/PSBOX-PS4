#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static checks of selected source; never builds or contacts a service."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit
sys.dont_write_bytecode = True
from public_tree import selected_files, is_public_path, credential_findings

REQUIRED = ['ps4-homebrew/CMakeLists.txt', 'README.md', 'BUILDING.md', 'VERSION', 'BUILD_NUMBER', 'AGENTS.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md',
            'BUILD-PREREQUISITES.md', 'DEPENDENCIES.md', 'docs/SOURCE-PROVENANCE.md',
            'docs/LICENSE-STATUS.md', 'docs/PUBLICATION.md', 'docs/external-inputs.json',
            'deps/opus/CMakeLists.txt', 'deps/opus/COPYING', 'licenses/README.md',
            'licenses/references/Greenlight-MIT.txt', 'licenses/libdatachannel-LICENSE.txt',
            'licenses/MbedTLS-LICENSE.txt', 'licenses/Opus-COPYING.txt',
            'ps4-homebrew/audio_player.cpp', 'ps4-homebrew/video_color.h',
            'src/platform/ps4_build_info.h',
            'ps4-homebrew/stream_menu_ui.cpp', 'ps4-homebrew/stream_stats_ui.cpp']

def check(root: Path, verify_manifest: bool = True) -> tuple[list[str], int]:
    errors = []
    files = selected_files(root)
    chosen = {str(p) for p in files}
    for name in REQUIRED:
        if name not in chosen:
            errors.append(f'Missing selected source/notice: {name}')
    # Audit every tracked path, not just a selected subset of a checkout.
    if (root / '.git').exists():
        import subprocess
        tracked = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z'])
        for name in tracked.split(b'\0'):
            if name and not is_public_path(Path(name.decode())):
                errors.append('Excluded tracked file: ' + name.decode())
    # An export/archive must contain ONLY approved files, not just select a subset.
    if not (root / '.git').exists():
        for p in root.rglob('*'):
            if (p.is_file() or p.is_symlink()) and not is_public_path(p.relative_to(root)):
                errors.append(f'Excluded file found in source export: {p.relative_to(root)}')
    for rel in files:
        p = root / rel
        if p.is_symlink() or any(parent.is_symlink() for parent in p.parents if parent != root):
            errors.append(f'Symlink is not allowed in public source: {rel}')
            continue
        data = p.read_bytes()
        for label, line in credential_findings(rel, data):
            errors.append(f'Credential candidate ({label}): {rel}:{line}; value suppressed')
        if rel.suffix == '.py' and rel.parts[0] not in {'deps', 'extern'}:
            try:
                ast.parse(data, filename=str(rel))
            except (SyntaxError, UnicodeError) as exc:
                errors.append(f'Python syntax: {rel}:{getattr(exc, "lineno", 0)}')
        if rel.suffix == '.md' and rel.parts[0] not in {'deps', 'extern', 'licenses'}:
            text = data.decode('utf-8')
            for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', text):
                target = target.strip().split(' "', 1)[0].strip('<>')
                parsed = urlsplit(target)
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue
                local = (p.parent / unquote(parsed.path)).resolve()
                if not local.is_relative_to(root) or not local.exists():
                    errors.append(f'Broken/nonpublic documentation link: {rel} -> {target}')
            if re.search(r'/(?:home|run/media)/[^/\s]+/', text):
                errors.append(f'Workstation-specific path in public documentation: {rel}')
    cmake_path = root / 'ps4-homebrew/CMakeLists.txt'
    if not cmake_path.is_file() or cmake_path.is_symlink():
        return errors, len(files)
    cmake = cmake_path.read_text()
    if 'instrucoes privadas' in cmake:
        errors.append('CMake still depends on private instructions')
    block = re.search(r'add_executable\(xcloud_homebrew\s+(.*?)\)\s*target_include', cmake, re.S)
    if not block:
        errors.append('Cannot inspect active CMake source list')
    else:
        for source in block.group(1).split():
            source = source.strip('"')
            if '$' in source:
                continue  # external SDK sources are checked by build.sh --check-inputs
            p = (root / 'ps4-homebrew' / source).resolve()
            if not p.is_file() or not p.is_relative_to(root) or str(p.relative_to(root)) not in chosen:
                errors.append(f'Missing active source in public selection: {source}')
    workflow = root / '.github/workflows/build-ps4.yml'
    if not workflow.is_file():
        errors.append('Missing source-only CI workflow')
    else:
        text = workflow.read_text()
        for marker in ['gh release download', 'workstation-backup-', 'upload-artifact', 'tools/ps4/build.sh']:
            if marker in text:
                errors.append(f'Public workflow contains private/binary action: {marker}')
    manifest = root / 'SOURCE-MANIFEST.json'
    if verify_manifest and manifest.exists():
        records = json.loads(manifest.read_text())['files']
        listed = {entry['path'] for entry in records}
        if listed != chosen - {'SOURCE-MANIFEST.json'}:
            errors.append('Source manifest file set differs from selected tree')
        for entry in records:
            rel = Path(entry['path'])
            if not is_public_path(rel):
                errors.append('Manifest contains an excluded path')
                continue
            p = root / rel
            if not p.is_file() or p.is_symlink():
                errors.append(f'Missing/unsafe manifest file: {rel}')
                continue
            if p.stat().st_size != entry['size'] or hashlib.sha256(p.read_bytes()).hexdigest() != entry['sha256']:
                errors.append(f'Manifest content mismatch: {rel}')
    return errors, len(files)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    errors, count = check(args.root.resolve())
    if errors:
        for error in errors:
            print('ERROR:', error, file=sys.stderr)
        return 1
    print(f'PASS: {count} selected public files; notices, links, active inputs and credential patterns checked.')
    print('Static source check only; Git history, remote artifacts and binary rights are not cleared.')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
