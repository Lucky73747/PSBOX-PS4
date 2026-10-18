# Source origins and modifications

## Application base

PSBOX-PS4 is a PS4 port/derivative of [RafaelNGP/xbox-cloud-gaming-ps5](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5), GPL-3.0-or-later. Original RafaelNGP copyright and SPDX notices remain in the shared sources. The comparison reference used during preparation was local upstream revision `f8b2726`; this identifies the comparison snapshot, not a proven fork-point of every file.

The active PS4 CMake target compiles shared UI, authentication, catalog, WebRTC/session, RTP/RTCP and input-packet sources from `src/`. The PS4 layer adds native platform glue, Videodec2 video handling, Vulkan/VideoOut composition, libopus/SDL3 audio, stream menus/statistics and PS4 build integration. Changes were made to shared sources for PS4 transport, diagnostics, rendering, caching and UI. This is not a claim that all PS4 work is independently authored or that upstream shared code was authored by the port contributors.

Legacy PS5 modules remain in `src/media/`, `src/display/` and platform directories. In particular, the inherited `src/media/hw_decoder.cpp` and FFmpeg wrappers are not the decoder chosen by the active PS4 target. The active decoder is `ps4-homebrew/video_decoder.cpp`; the active audio adapter is `ps4-homebrew/audio_player.cpp`.

## Technical references

- [Greenlight](https://github.com/unknownskl/greenlight), MIT, UnknownSKL/Jim Kroon, and [xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player) informed xCloud protocol understanding. The shared packet/message code follows the service's wire format; matching protocol fields are not in themselves proof of copied implementation. A comparison against Greenlight revision `0bf80599bf7c9c102a939ce994b5118cd7cb57ce` found no identical first-party files or long literal blocks; it cannot exclude translated/adapted code. Do not infer independent authorship from that test. Greenlight's MIT notice is retained in `licenses/references/Greenlight-MIT.txt` as a conservative attribution.
- [JaimeJimenezG/Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4), comparison revision `61427a214d4e632ee246816a98ee4f2374844a73`, was consulted for PS4 video behavior. The PS4 decoder's NV12 visibility comment explicitly references the local Moonlight path. No complete decoder transplant was evidenced by the comparison; do not infer permission to reuse Moonlight code from its public availability or from dependencies' licenses. Another historical reference named `scenee/Moonlight-PS4` is a different repository and must not be substituted for this one.
- Legacy PS5 hardware decoder comments credit BlackBearReloaded's [ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight) and [PS5 hardware video decoding research](https://github.com/blackbearreloaded/ps5-hardware-video-decoding-research), GPL-3.0-or-later.
- FSR1, Anime4K, fonts and libraries retain their component notices. See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

The review compares local snapshots, not the full authorship history or every previous upstream version. Preserve all existing per-file copyright/permission notices and document further modifications. If a nontrivial borrowed implementation is identified later, record its exact origin and retain its applicable license rather than describing it only as a reference.
