# Contributor instructions

- PSBOX is a PS4 port of RafaelNGP's xbox-cloud-gaming-ps5. Preserve upstream copyright/SPDX notices and document modifications.
- The active target is `ps4-homebrew/`; `src/` also contains shared and legacy PS5 code. Consult `docs/SOURCE-PROVENANCE.md` before treating legacy files as active PS4 code.
- Inspect Git status and preserve existing changes and persistent build caches. Do not discard user changes or run a clean build without a reason.
- Follow any local-only instructions that are present, but do not commit or export them.
- Compile the PS4 app or produce a PKG only when explicitly requested. Host UI previews are separate and do not validate PS4 performance.
- Keep `VERSION` (semantic app version) and `BUILD_NUMBER` (monotonic source build identity) as the source of truth. The PS4 package/SFO version is a separate installer version and must remain monotonic for updates.
- Do not install, deploy, publish, push, change repository visibility, or upload release artifacts without explicit authorization for that action.
- Keep credentials, account files, console logs, dumps, external SDK bundles and local notes out of public source. Review both history and remote releases before making an existing repository public.
- Use `python3 tools/release/check-public-tree.py` for source checks and `python3 tools/release/export-public-source.py --output /path/outside/checkout` for a history-free local export.
- Run relevant checks after edits. Keep documentation, build paths, notices and package contents consistent. Source checks do not establish runtime correctness or binary redistribution clearance.
