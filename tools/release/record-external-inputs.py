#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hash selected local build inputs without redistributing them or local paths."""
import argparse
from datetime import date
import hashlib
import json
import os
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=root / 'docs/external-inputs.json')
    args = parser.parse_args()
    deps = root / 'dependencies'
    sdk = Path(os.environ.get('PS4_ORBIS_SDK_BUNDLE', deps / 'orbis-sdk-v1/orbis-sdk-v1'))
    sources = {
        'sdk-bundle': sdk,
        'openorbis-toolchain': Path(os.environ.get('OO_PS4_TOOLCHAIN', deps / 'OpenOrbis-PS4-Toolchain')),
        'orbis-compat': Path(os.environ.get('ORBIS_COMPAT_DIR', sdk / 'orbis-compat')),
        'mesa-build': Path(os.environ.get('ORBIS_MESA_BUILD', Path(os.environ.get('ORBIS_MESA_SRC', sdk / 'mesa')) / 'build-orbis')),
        'sdl3-build': Path(os.environ.get('XC_PS4_SDL3_BUILD', deps / 'SDL-build-sdl3-freebsd')),
        'sdl3-source': Path(os.environ.get('XC_PS4_SDL3_SOURCE', deps / 'SDL')),
        'orbis-porting-kit': Path(os.environ.get('ORBIS_KIT_DIR', deps / 'orbis-porting-kit')),
    }
    selected = {
        'sdk-bundle': ['toolchain/orbis-sdk.cmake', 'NOTICE.md', 'BUNDLE.txt', 'mesa/manifest.txt', 'sdk/lib/*.a'],
        'openorbis-toolchain': ['link.x', 'LICENSE'],
        'orbis-compat': ['optional/orbis_thread_atexit_stub.c', 'LICENSE'],
        'mesa-build': ['src/amd/vulkan/libvulkan_radeon.a', 'subprojects/zlib-*/libz.a'],
        'sdl3-build': ['libSDL3.a', 'include-config-release/**/*.h', 'include-revision/**/*.h'],
        'sdl3-source': ['LICENSE.txt', 'include/SDL3/SDL_version.h', 'src/audio/ps4/*'],
        'orbis-porting-kit': ['LICENSE', 'vkloader/*.c', 'vkloader/*.h', 'vkloader/CMakeLists.txt'],
    }
    entries = []
    for component, base in sources.items():
        if not base.is_dir():
            raise SystemExit(f'Missing external input directory: {component}')
        paths = sorted({p for pattern in selected[component] for p in base.glob(pattern) if p.is_file()})
        if not paths:
            raise SystemExit(f'No expected files found: {component}')
        for p in paths:
            h = hashlib.sha256()
            with p.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024*1024), b''):
                    h.update(chunk)
            entries.append({'component': component, 'path_in_component': str(p.relative_to(base)),
                            'size': p.stat().st_size, 'sha256': h.hexdigest()})
    data = {'format': 1, 'recorded': date.today().isoformat(),
            'scope': 'Selected local link inputs, headers and notices; not a complete source/reproducibility manifest',
            'external_files_redistributed': False,
            'source_availability': 'External sources/patches and unresolved licensing require separate review before any binary release; see LICENSE-STATUS.md',
            'files': entries}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2)+'\n')
    print(f'Recorded {len(entries)} selected external-input hashes; no external binaries copied.')

if __name__ == '__main__':
    main()
