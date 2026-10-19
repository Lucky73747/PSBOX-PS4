# Building PSBOX for PS4

PSBOX's app version is recorded in `VERSION`; `BUILD_NUMBER` identifies a source build. The current development line is version 0.1.1, build 11. The PS4 title ID is `PSBX00001`. The installer version is managed separately in `ps4-homebrew/CMakeLists.txt` and must increase for packages intended to update an existing installation.

## Requirements and local build

Install the inputs listed in [BUILD-PREREQUISITES.md](BUILD-PREREQUISITES.md). External SDK and library bundles are not included in this repository. Keep the existing build directory for incremental builds.

```sh
tools/ps4/build.sh --check-inputs
tools/ps4/build.sh
```

The default library interface is `NEW`; the previous interface remains available with `PSBOX_LIBRARY_LAYOUT=LEGACY`:

```sh
PSBOX_LIBRARY_LAYOUT=LEGACY tools/ps4/build.sh
```

The script creates a full PKG in `build-ps4-homebrew/`. A successful build confirms compilation and packaging only; test installation and runtime behavior on a PS4 separately. The build script does not install or deploy the package.

## Library UI and artwork cache

The new library interface is the default for PS4. Catalog artwork is requested and decoded asynchronously through the existing image cache. The cache persists downloaded image data under `/data/PSBOX/image-cache-v2`; placeholders remain visible while artwork loads. Recent games are kept in memory for the current app run.

With Vulkan active, supported shapes and text are composed through Vulkan. Gradients and artwork are prepared by the CPU Canvas, so the interface is GPU-composited but not fully GPU-rendered. If renderer initialization fails, the CPU Canvas fallback is used. Stream settings remain in `/data/PSBOX/stream_profile.json`.

## GitHub Actions

The manual **Compile PS4 beta** workflow uses the public source plus private build inputs stored outside this repository. Configure `PSBOX_BUILD_INPUTS_URL` as an Actions secret and `PSBOX_BUILD_INPUTS_SHA256` as an Actions variable; use a temporary download URL and renew it before a later run. Never commit the URL, SDK bundles, credentials, account files or console logs.

Actions uploads the PKG, its SHA-256 checksum and build identity as a temporary artifact. It does not install the app on a console or publish a release. Review [docs/LICENSE-STATUS.md](docs/LICENSE-STATUS.md) before distributing binaries.
