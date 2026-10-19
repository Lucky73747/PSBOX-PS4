# Source and binary distribution status

## Public source preparation

The application is GPL-3.0-or-later, derived from RafaelNGP's PS5 project. Public source retains its copyright/SPDX notices, vendored source trees and licenses, and includes current PS4 modules. Private instructions, credentials, logs, build products, external SDK inputs and Git history are excluded from the history-free source export. Source export does not publish anything remotely.

## Current PSBOX 0.1.1 candidate package inventory

The candidate package contains `sce_module/libc.prx` and `sce_module/libSceFios2.prx`, generated from the OpenOrbis PS4 Toolchain v0.5.4 modules. The exact upstream module source, project/build files and GPL-3.0 license are now included under `licenses/corresponding-source/OpenOrbis-PS4-Toolchain-v0.5.4/`; the directory README maps package paths to PRX SHA-256 values. These files were byte-compared with upstream tag `v0.5.4` (commit `b458dfd9d2f5c40aa249b127c9b9b4488afdc686`). The OpenOrbis SDK/toolchain itself is not included in the public source or package.

This closes the corresponding-source gap for those two PRX modules. It does **not** clear the package for redistribution: the SDK notice still identifies unresolved provenance/licensing for imported stubs and some SDK inputs. Those inputs are build-time and are not copied into the package as standalone files, but their role in producing the final executable and any applicable terms still require a project owner review. SDL3 is an external build input and its exact source/build provenance must also be recorded for a complete binary release record.

## Before distributing a PKG or SDK bundle

The following are **not cleared by preparing the public source**:

1. Obtain the exact complete sources and local modifications for external SDL3/PS4, Orbis/Mesa/compat and other linked inputs; retain source, build/installation materials and notices required by their terms. Local archive/header hashes alone are not corresponding source. The two included OpenOrbis PRXs now have their pinned corresponding source in this repository; other external inputs remain to be reviewed.
2. Review the exact SDK's component licensing, exceptions and unresolved items. The supplied bundle's `NOTICE.md` explicitly lists unresolved SDK stub/header provenance. `licenses/external/` preserves that notice; it is not a blanket license or permission to redistribute the SDK.
3. Package applicable complete licenses/notices for the actual link graph, and make required corresponding source available for the precise binary. CMake now copies the public notice collection into newly built packages, but old PKGs are unchanged and their contents have not been retroactively fixed.
4. Record the exact application source manifest/commit, toolchain and external inputs, compile the selected source, and validate the resulting package on hardware. Recent UI/statistics edits are not hardware-validated by source checks.
5. Review every release artifact separately. A source review does not establish redistribution rights for a compiled package. Do not include credentials, raw logs, or diagnostic captures in a public release.

The GPL text in `LICENSE` does not replace third-party terms, and including a text does not resolve missing permission/source. This is a project release checklist, not a legal clearance for an unaudited SDK or binary.
