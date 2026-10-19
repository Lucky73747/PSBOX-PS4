# Source and binary distribution status

## Public source preparation

The application is GPL-3.0-or-later, derived from RafaelNGP's PS5 project. Public source retains its copyright/SPDX notices, vendored source trees and licenses, and includes current PS4 modules. Private instructions, credentials, logs, build products, external SDK inputs and Git history are excluded from the history-free source export. Source export does not publish anything remotely.

## PSBOX 0.1.1 beta b18 package inventory

The candidate package contains `sce_module/libc.prx` and `sce_module/libSceFios2.prx`, generated from the OpenOrbis PS4 Toolchain v0.5.4 modules. The exact upstream module source, project/build files and GPL-3.0 license are now included under `licenses/corresponding-source/OpenOrbis-PS4-Toolchain-v0.5.4/`; the directory README maps package paths to PRX SHA-256 values. These files were byte-compared with upstream tag `v0.5.4` (commit `b458dfd9d2f5c40aa249b127c9b9b4488afdc686`). The OpenOrbis SDK/toolchain itself is not included in the public source or package.

The package also statically links SDL3, Mesa RADV, zlib, orbis-compat and the orbis-porting-kit Vulkan loader. The release includes a separate corresponding-source archive with the exact SDL source tree used by the saved build and source snapshots for the other linked components. Its README identifies the snapshot/revision for each component and the package/source commit it accompanies. The PSBOX source archive provides the application source and the two PRX module sources.

The PS4 PKG does **not** contain the SDK bundle, its import-stub libraries, or host-side build/packaging tools. The SDK notice in `licenses/external/` documents the separate local build bundle; it is not a notice granting permission to redistribute that bundle. PS4 platform system libraries are not shipped as files in this PKG. The release source archive is limited to source and license materials for the application and linked redistributable components; it is not a replacement SDK.

The corresponding-source and notice files are prepared for this specific b18 package. This repository review found no package-level license conflict requiring the PKG to be withheld, provided the package and source/notices are distributed together as listed in the release. This is a project inventory, not legal advice or a guarantee about separate SDK bundles or other builds.

## Distribution scope and remaining checks

The following are **not cleared by preparing the public source**:

1. Keep the matching PKG, corresponding-source archive, `BUILD.txt`, and `SHA256SUMS` together. Do not reuse the source archive for a different build without checking the link inputs and refreshing its manifest.
2. Do not publish or redistribute the local SDK/toolchain bundle based on this package review. Its separate notice contains unresolved provenance items, and it is excluded from the release assets.
3. The source manifest/checker verifies the source export's file set; it does not validate console runtime behavior. The b18 candidate has prior owner-reported console validation; any added package payload should be retested by the owner if desired.
4. Review each later build independently. Do not include credentials, raw logs, or diagnostic captures in public release assets.

The GPL text in `LICENSE` does not replace third-party terms, and including a text does not resolve missing permission/source. This is a project release checklist, not a legal clearance for an unaudited SDK or binary.
