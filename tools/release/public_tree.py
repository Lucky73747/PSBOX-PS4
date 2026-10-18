# SPDX-License-Identifier: GPL-3.0-or-later
"""Select public source without exporting Git history or local material."""
from pathlib import Path
import re
import subprocess

PUBLIC_DIRS = {'.github', 'assets', 'cmake', 'deps', 'docs', 'extern', 'licenses',
               'ps4-homebrew', 'src', 'tools'}
PUBLIC_FILES = {'.gitignore', 'AGENTS.md', 'README.md', 'LICENSE',
                'THIRD_PARTY_NOTICES.md', 'BUILD-PREREQUISITES.md',
                'BUILDING.md', 'VERSION', 'BUILD_NUMBER', 'DEPENDENCIES.md', 'DEPENDENCIES-RECOVERY.md', 'BETA_TESTING.md',
                'SOURCE-MANIFEST.json'}
FORBIDDEN_PARTS = {'.git', '__pycache__', 'node_modules', '.venv', 'venv', '.DS_Store'}
FORBIDDEN_SUFFIXES = {'.pkg', '.elf', '.prx', '.log', '.ppm', '.aus', '.h264',
                      '.vdp', '.dmp', '.core', '.pyc', '.pyo', '.a', '.o', '.so'}
ACCOUNT_NAMES = {'account.json', 'msa_refresh_token.txt', 'autoplay.txt'}
HIGH_SIGNAL = {
    'github-token': re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b'),
    'aws-access-id': re.compile(rb'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
}
FIRST_PARTY = {
    'jwt': re.compile(rb'\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{10,}\b'),
    'private-key': re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----'),
    'xbox-user-authorization': re.compile(rb'XBL3\.0 x=[^\s"\r\n]{15,}'),
    'msa-refresh-token': re.compile(rb'\bM\.[A-Za-z0-9_!.*-]{60,}'),
    'credential-url': re.compile(rb'https?://[^\s/:@]{2,}:[^\s/@]{4,}@'),
}

def is_public_path(path: Path) -> bool:
    parts = path.parts
    if not parts or path.is_absolute() or '..' in parts:
        return False
    if any(x in FORBIDDEN_PARTS for x in parts):
        return False
    if any('privad' in x.casefold() for x in parts) or path.name.casefold().endswith('.local.md'):
        return False
    if parts[0] not in PUBLIC_DIRS and not (len(parts) == 1 and parts[0] in PUBLIC_FILES):
        return False
    if path.name.lower() in ACCOUNT_NAMES or path.name == '.env' or path.name.startswith('.env.'):
        return False
    if path.suffix.lower() in FORBIDDEN_SUFFIXES:
        return False
    if parts[:3] == ('tools', 'ps4', 'ui-preview') and path.name.startswith('preview') and path.suffix == '.png':
        return False
    if path.as_posix() == 'tools/ps4/ps4-pc-manager/ps4-pc-manager.desktop':
        return False
    if len(parts) >= 3 and parts[:2] == ('.github', 'workflows') and 'private' in path.stem.casefold():
        return False
    return True

def selected_files(root: Path) -> list[Path]:
    if (root / '.git').exists():
        # Tracked files must remain audited even when an ignore rule matches them.
        # Ignore rules apply only to untracked candidates; deleted files are omitted.
        raw = subprocess.check_output(['git', '-C', str(root), 'ls-files',
                                       '--cached', '--others', '--exclude-standard', '-z'])
        paths = [Path(x.decode('utf-8')) for x in raw.split(b'\0') if x]
        candidates = sorted({p for p in paths if is_public_path(p)}, key=str)
        return [p for p in candidates if (root / p).is_file() or (root / p).is_symlink()]
    return sorted((p.relative_to(root) for p in root.rglob('*')
                   if (p.is_file() or p.is_symlink()) and is_public_path(p.relative_to(root))), key=str)

def credential_findings(path: Path, data: bytes) -> list[tuple[str, int]]:
    if b'\0' in data[:8192]:
        return []
    rules = dict(HIGH_SIGNAL)
    # Vendored cryptography fixtures/PEM parser strings are intentionally public.
    if path.parts[0] not in {'deps', 'extern'}:
        rules.update(FIRST_PARTY)
    return [(label, data.count(b'\n', 0, match.start()) + 1)
            for label, pattern in rules.items() for match in pattern.finditer(data)]
