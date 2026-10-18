# Source and binary distribution status

## Public source preparation

The application is GPL-3.0-or-later, derived from RafaelNGP's PS5 project. Public source retains its copyright/SPDX notices, vendored source trees and licenses, and includes current PS4 modules. Private instructions, credentials, logs, build products, external SDK inputs and Git history are excluded from the history-free source export. Source export does not publish anything remotely.

## Before distributing a PKG or SDK bundle

The following are **not cleared by preparing the public source**:

1. Obtain the exact complete sources and local modifications for external SDL3/PS4, Orbis/Mesa/compat and other linked inputs; retain source, build/installation materials and notices required by their terms. Local archive/header hashes alone are not corresponding source.
2. Review the exact SDK's component licensing, exceptions and unresolved items. The supplied bundle's `NOTICE.md` explicitly lists unresolved SDK stub/header provenance. `licenses/external/` preserves that notice; it is not a blanket license or permission to redistribute the SDK.
3. Package applicable complete licenses/notices for the actual link graph, and make required corresponding source available for the precise binary. CMake now copies the public notice collection into newly built packages, but old PKGs are unchanged and their contents have not been retroactively fixed.
4. Record the exact application source manifest/commit, toolchain and external inputs, compile the selected source, and validate the resulting package on hardware. Recent UI/statistics edits are not hardware-validated by source checks.
5. Review every release artifact separately. A source review does not establish redistribution rights for a compiled package. Do not include credentials, raw logs, or diagnostic captures in a public release.

The GPL text in `LICENSE` does not replace third-party terms, and including a text does not resolve missing permission/source. This is a project release checklist, not a legal clearance for an unaudited SDK or binary.
