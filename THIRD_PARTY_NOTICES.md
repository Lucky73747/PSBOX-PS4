# Third-party notices

PSBOX application code is licensed under GNU GPL version 3 or, at your option,
any later version (see `LICENSE`). This repository retains both PS4 and legacy
PS5 project material. Third-party components keep their own license terms; the
PS4 and PS5 target inventories are separated below. Check the package recipe
and [docs/LICENSE-STATUS.md](docs/LICENSE-STATUS.md) before redistributing either target.

When distributing a binary, provide the corresponding source and build
materials required by each applicable license. The needed external SDK and
toolchain inputs differ between the PS4 and PS5 targets; see the sections
below and [docs/LICENSE-STATUS.md](docs/LICENSE-STATUS.md) for the current inventory and gaps.


## Application origin and PS4 changes

PSBOX-PS4 is a PS4 port/derivative of [RafaelNGP/xbox-cloud-gaming-ps5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5), GPL-3.0-or-later. Shared authentication, catalog, UI and streaming code retains RafaelNGP copyright notices. PSBOX contributors added/modified PS4 platform integration, native video/audio, rendering, menus, diagnostics and build integration. Legacy PS5 code is retained in the source tree but is not all compiled by the PS4 target. See [docs/SOURCE-PROVENANCE.md](docs/SOURCE-PROVENANCE.md).

Moonlight-ps4 was consulted for PS4 video behavior; the NV12 visibility code explicitly mentions that reference. This inventory does not claim independent authorship of every line, or license Moonlight's own code. Component and reference license texts are retained under `licenses/`.

## Legacy PS5 target inventory (not the active PS4 link graph)

This historical inventory is retained to explain legacy code origins. Some PS5-only paths/tools belong to the upstream project and are absent from this PS4 source export. It does not describe the active PS4 package.

| Component | Version | License | Source / license text |
| --- | --- | --- | --- |
| Mbed TLS | 3.6.2 | Apache-2.0 (dual-licensed Apache-2.0 OR GPL-2.0-or-later) | `deps/mbedtls`, `deps/mbedtls/LICENSE` |
| libdatachannel | 0.23.1 | MPL-2.0 | `deps/libdatachannel`, `LICENSE` there |
| libjuice | b509b6c (libdatachannel submodule) | MPL-2.0 | `deps/libdatachannel/deps/libjuice` |
| usrsctp | fec583d (libdatachannel submodule) | BSD-3-Clause | `deps/libdatachannel/deps/usrsctp/LICENSE.md` |
| libSRTP | ee1a77c (libdatachannel submodule) | BSD-3-Clause | `deps/libdatachannel/deps/libsrtp/LICENSE` |
| plog | 94899e0 (libdatachannel submodule) | MIT | `deps/libdatachannel/deps/plog/LICENSE` |
| FFmpeg (libavcodec, libswresample, libavutil; H.264 and Opus decoders only) | n7.1.2 | LGPL-2.1-or-later (built without `--enable-gpl`/`--enable-nonfree`) | `deps/ffmpeg`, `deps/ffmpeg/COPYING.LGPLv2.1`; build flags in `tools/build-ffmpeg.sh` |
| stb_truetype, stb_image, stb_image_resize2 | upstream master (2026) | MIT or public domain (Unlicense), at your choice | `extern/stb`, `extern/stb/LICENSE` |
| QR Code generator library (C) | upstream master (2026) | MIT | `extern/qrcodegen` (license in the headers) |
| PS5 app runtime: `app_crt.cpp`, `app_cpp_runtime.cpp` (BlackBearReloaded, ps5-native-app-boilerplate) | from PS5_Vulkan | GPL-3.0-or-later | PS5_Vulkan `tooling/native/` |
| PS5 payload SDK platform layer (`libps5platform.a`) and headers | SDK v0.42 + PS5_Vulkan fork | GPL-3.0-or-later | <https://github.com/ps5-payload-dev/sdk> |
| LLVM libc++, libc++abi, libunwind, compiler-rt builtins (from the payload SDK) | SDK v0.42 | Apache-2.0 WITH LLVM-exception | the payload SDK |
| Mesa: RADV Vulkan driver with ACO and NIR, PS5 winsys (PS5_Mesa fork, built by PS5_Vulkan `tools/build-radv.sh`) | Mesa 26.2.0, PS5_Mesa 0b2d6d1 | MIT (individual files per their SPDX identifiers) | <https://github.com/mihawk-99/PS5_Mesa>, `docs/license.rst` and `licenses/` there |

## PS4 target components

The active PS4 target is defined by `ps4-homebrew/CMakeLists.txt`.

| Component | Version / source | License / notice |
| --- | --- | --- |
| Mbed TLS | 3.6.2, `deps/mbedtls` | Apache-2.0 OR GPL-2.0-or-later; source offers both. The PS4 build uses the Apache-2.0 option. Full text in `deps/mbedtls/LICENSE`. |
| libdatachannel | 0.23.1, `deps/libdatachannel` | MPL-2.0; full text in `deps/libdatachannel/LICENSE`. |
| libjuice | bundled with libdatachannel | MPL-2.0; `deps/libdatachannel/deps/libjuice/LICENSE`. |
| usrsctp | bundled with libdatachannel | BSD-3-Clause; `deps/libdatachannel/deps/usrsctp/LICENSE.md`. |
| libsrtp | bundled with libdatachannel | BSD-3-Clause; `deps/libdatachannel/deps/libsrtp/LICENSE`. |
| plog | bundled with libdatachannel | MIT; `deps/libdatachannel/deps/plog/LICENSE`. |
| nlohmann/json | bundled headers | MIT and other retained notices; see `deps/libdatachannel/deps/json/LICENSES/`. |
| qrcodegen | `extern/qrcodegen` | MIT; copyright and permission notice in source headers and README. |
| stb_image, stb_image_resize2, stb_truetype | `extern/stb` | MIT or Unlicense; see `extern/stb/LICENSE` and file headers. |
| SDL3 | External PS4 build input | zlib license; exact source/build and notice must accompany a redistributable binary. |
| Opus | 1.5.2, upstream tag `v1.5.2`, commit `ddbe48383984d56acd9e1ab6a090c54ca6b735a6`, `deps/opus` | BSD-3-Clause; full notice in `deps/opus/COPYING`, embedded in the PS4 PKG as `licenses/Opus-COPYING`. |
| Mesa RADV, zlib, OpenOrbis SDK/toolchain, orbis-porting-kit | External PS4 build inputs | Preserve the exact bundle's license, exception, notice, and per-file SPDX texts; there is no single blanket license. |
| AMD FidelityFX FSR1 | `extern/fsr1` | MIT; `extern/fsr1/LICENSE.txt`. |
| Inter fonts | `assets/fonts` | SIL Open Font License 1.1; `assets/fonts/OFL.txt`. |
| PSBOX icon/artwork | `assets/ps4/icon0.png`, project UI sources | Project code/artwork notice; GPL-3.0-or-later. |

The PS4 recipe copies this notice, the project license, font notices and the public `licenses/` collection into newly built packages. Old PKGs are not changed. This improves notice coverage but does not resolve corresponding-source or unresolved external SDK terms. See [docs/LICENSE-STATUS.md](docs/LICENSE-STATUS.md) before distributing a binary.

## PS5 files shipped in the app package

| File | License |
| --- | --- |
| `sce_module/libc.prx` (generated runtime module, BlackBearReloaded) | GPL-3.0-or-later; see PS5_Vulkan `runtime/README.md` |
| `assets/fonts/Inter-*.ttf` (Inter 4.1, Rasmus Andersson) | SIL Open Font License 1.1 (`assets/fonts/OFL.txt`) |
| `assets/cacert.pem` (Mozilla CA certificate data, from <https://curl.se/ca/cacert.pem>, 2026-09-25) | MPL-2.0 |
| `sce_sys/icon0.png` and the in-app logo | Project artwork, GPL-3.0-or-later (`src/ui/brand.cpp`) |

## PS5 tools used at build time only

| Tool | License |
| --- | --- |
| LLVM/Clang/lld | Apache-2.0 WITH LLVM-exception |
| NASM (assembles FFmpeg's x86 SIMD code) | BSD-2-Clause |
| PS5_Vulkan tooling (`link.sh` recipe, `ps5-native-tool` ELF/FSELF writer, FTP deploy script) | GPL-3.0-or-later |

## Protocol references

The xCloud protocol handling was informed by the open-source clients
[xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player) and
[Greenlight](https://github.com/unknownskl/greenlight) (MIT). They were consulted as protocol references. Local comparisons did not establish literal implementation copying; they do not prove the absence of translated/adapted code. See [docs/SOURCE-PROVENANCE.md](docs/SOURCE-PROVENANCE.md). Greenlight's MIT notice is preserved under `licenses/references/`.

## Trademarks

Xbox, Xbox Cloud Gaming and Game Pass are trademarks of the Microsoft group
of companies. PlayStation and PS5 are trademarks of Sony Interactive
Entertainment Inc. This project is not affiliated with, endorsed by or
sponsored by Microsoft or Sony. Game names and box art shown in the app are
loaded at runtime from Microsoft's public catalog and belong to their owners;
none are distributed with this project.

## AMD FidelityFX Super Resolution 1.0 (FSR 1)

`extern/fsr1/ffx_a.h` and `extern/fsr1/ffx_fsr1.h`, compiled into the shaders in
`ps4-homebrew/shaders/` and the PS5 shader tree (EASU upscaling, RCAS sharpening). MIT License,
Copyright (c) 2021 Advanced Micro Devices, Inc.: see `extern/fsr1/LICENSE.txt`.

## References

- `src/media/hw_decoder.cpp` declares libSceVideodec2's structures as the PS5
  lays them out, and sets the decoder up (sysmodule 207, memory type 12,
  compute queue 0/0) after BlackBearReloaded's
  [ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) and
  [ps5-hardware-video-decoding-research](https://github.com/blackbearreloaded/ps5-hardware-video-decoding-research)
  (both GPL-3.0-or-later).

## Anime4K

`extern/anime4k/` (bloc97, MIT, see `extern/anime4k/LICENSE`): the
Upscale_CNN_x2 networks and Restore_CNN_S (v4.0.1); `tools/gen-anime4k.py`
turns their passes, weights unchanged, into `src/display/shaders/a4k_*.comp`. Brought to our attention by
sainsaji's EVO Player for PS5, which runs them in its own pipeline.

## PSBOX application icon

`assets/ps4/icon0.svg` is the editable source of the original PSBOX monochrome screen-and-controls icon. `icon0.png` is its 512×512 package export. Created for this PS4 project and distributed under GPL-3.0-or-later. It replaces the upstream application artwork and contains no Xbox or PlayStation logos.
