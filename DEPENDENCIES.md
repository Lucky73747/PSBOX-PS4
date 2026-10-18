# Dependencies and build inputs

## Vendored sources

The public source includes libdatachannel and nested dependencies, Mbed TLS, Opus, qrcodegen, stb, FSR1, Anime4K and Inter font assets. License notices are retained in their trees and summarized in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). These are full vendored source trees, not parent-repository Git submodules; do not expect `git submodule update` to install the external PS4 SDK.

The snapshot's recorded base revisions are:

| Component | Recorded base identity |
| --- | --- |
| libdatachannel | 0.23.1, `222529eb2c8ae44f96462504ae38023f62809cec` |
| libjuice | `b509b6c1f90e4a1d4753e6bde62ffbeb4d10c6bb` |
| usrsctp | `fec583d54493f879d2ae44a743423bf8a04371ab` |
| libsrtp | `ee1a77c9f9dc02c42bda9901038c500c5efe4cfa` |
| plog | `94899e0b926ac1b0f4750bfbd495167b4a6ae9ef` |
| nlohmann/json | `55f93686c01528224f448c19128836e7df245f72` |
| Mbed TLS | 3.6.2, `107ea89daaefb9867ea9121002fbbdf926780e98` |
| Opus | 1.5.2, `ddbe48383984d56acd9e1ab6a090c54ca6b735a6` |

These are base identities from the source inventory, not a claim that vendored trees are unmodified. The export's `SOURCE-MANIFEST.json` hashes the actual included files, including port changes.

## External PS4 inputs (not redistributed)

| Input | Recorded identity / gap |
| --- | --- |
| SDL3 PS4 | 3.4.10 base `e73c5b8a9aad712bdb9bfda1b3da2476ad0da890`; PS4 modifications/build must be supplied separately. |
| orbis-porting-kit | Base `213b0c38ff25ea7ca402c8844a6494086d0f1cae`; local modifications may exist. |
| Mesa | Bundle manifest: 26.3.0-devel, `1a59cc147b54e3970814b37313e2f27357672e9d`. The local bundle supplies selected headers/build products, not a complete Mesa source checkout. |
| orbis-compat | Bundle manifest: `314078413251644820955bd09335836ad19f88bd`. |
| OpenOrbis | SDK bundle references v0.5.4; exact input snapshots and their applicable notices remain important. |

The external build inputs are not bundled here. See [BUILD-PREREQUISITES.md](BUILD-PREREQUISITES.md) for configurable paths, [docs/external-inputs.json](docs/external-inputs.json) for recorded input hashes, and [docs/LICENSE-STATUS.md](docs/LICENSE-STATUS.md) for remaining binary release review. Hashes identify specific files; they do not make sources available or grant redistribution rights.

Public upstreams: [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain), [SDL](https://github.com/libsdl-org/SDL), [orbis-porting-kit](https://github.com/orbis-ports/orbis-porting-kit), [Mesa](https://docs.mesa3d.org/), [libdatachannel](https://github.com/paullouisageneau/libdatachannel), [Mbed TLS](https://github.com/Mbed-TLS/mbedtls), [Opus](https://github.com/xiph/opus).
