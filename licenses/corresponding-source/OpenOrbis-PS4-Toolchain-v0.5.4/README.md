# OpenOrbis PS4 Toolchain module source (v0.5.4)

This directory contains the corresponding source and project files for the two
OpenOrbis modules included in PSBOX PS4 packages:

| Package path | Module source | SHA-256 of distributed PRX |
| --- | --- | --- |
| `sce_module/libc.prx` | `src/modules/libc/` | `39ad53672bb0b14895f8465eb1619478ab935f5afe5dbf69c15f02cfc0a75ce6` |
| `sce_module/libSceFios2.prx` | `src/modules/libSceFios2/` | `3f8236c5996cf8e9917b8d27706d9743f6603d89de3b9859eacbb9f466b19c01` |

Upstream: <https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/tree/v0.5.4>
Tag commit: `b458dfd9d2f5c40aa249b127c9b9b4488afdc686`. The included
source/build files were byte-compared with that tag. The applicable upstream
GPL-3.0 license is included as `LICENSE`.

Each module's `lib.c` contains its dummy symbol. Its `Makefile` builds the
module with the OpenOrbis toolchain. From the module directory, with
`OO_PS4_TOOLCHAIN` set to an installed OpenOrbis toolchain, run `make` on Linux
or macOS. The Windows project and `build.bat` are also retained. The PRX hashes
above identify the package inputs; they are not claimed to be reproducible from
source without matching the exact compiler/toolchain inputs.

The SDK, compiler, import stubs, generated objects, ELF/OELF intermediates and
other toolchain binaries are not included here. See the root
`THIRD_PARTY_NOTICES.md` and `docs/LICENSE-STATUS.md` for their status.
