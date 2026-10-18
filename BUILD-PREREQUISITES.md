# PS4 build prerequisites

The source export includes the application and vendored source dependencies. Install these external inputs separately:

- CMake 3.20+, Ninja, Python 3, LLVM/Clang 21 and matching linker/tools.
- Compatible OpenOrbis SDK/toolchain and `toolchain/orbis-sdk.cmake`.
- Paired Orbis compatibility headers and Mesa/RADV archives, including zlib.
- orbis-porting-kit containing `vkloader/`.
- SDL3 PS4 sources, static archive and generated headers.

| Environment variable | Default / purpose |
| --- | --- |
| `PS4_ORBIS_SDK_BUNDLE` | `dependencies/orbis-sdk-v1/orbis-sdk-v1` |
| `OO_PS4_TOOLCHAIN` | `dependencies/OpenOrbis-PS4-Toolchain` |
| `ORBIS_KIT_DIR` | `dependencies/orbis-porting-kit` |
| `XC_PS4_SDL3_SOURCE` | `dependencies/SDL` |
| `XC_PS4_SDL3_BUILD` | `dependencies/SDL-build-sdl3-freebsd` |
| `ORBIS_COMPAT_DIR` | `$PS4_ORBIS_SDK_BUNDLE/orbis-compat` |
| `ORBIS_MESA_SRC` | `$PS4_ORBIS_SDK_BUNDLE/mesa` |
| `ORBIS_MESA_BUILD` | `$ORBIS_MESA_SRC/build-orbis` |
| `PSBOX_LLVM_BIN` | `/usr/lib/llvm-21/bin` |
| `PSBOX_BUILD_DIR` | `build-ps4-homebrew` (persistent incremental cache) |
| `PSBOX_BUILD_JOBS` | `2` |

Use `tools/ps4/build.sh --check-inputs` to verify required paths without configuring, compiling, packaging or accessing a console. The normal command configures CMake and builds `xcloud_homebrew`; it does not deploy it.

For host shader generation, install `glslangValidator`; `spirv-val` can validate generated SPIR-V. For the optional dock preview, use a C++20 compiler and a Python environment with PyQt6.

See [DEPENDENCIES.md](DEPENDENCIES.md) for source identities and limitations. Obtain all external inputs from sources that authorize their use and redistribution for your intended purpose.
