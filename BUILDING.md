# Building PSBOX for PS4

PSBOX's app version is recorded in `VERSION` and its source build number in `BUILD_NUMBER`. The beta starts at build 1, app version 0.1.0. Title ID `PSBX00001`, installer version `00.01`, installs separately from the old application.

## Local build

Install the host tools and obtain the compatible PS4 inputs listed in [BUILD-PREREQUISITES.md](BUILD-PREREQUISITES.md). The PS4 SDK, Mesa/RADV build products, porting kit and SDL3 PS4 port are not included in this source tree.

```sh
# Check required inputs and host tools without compiling:
tools/ps4/build.sh --check-inputs

# Configure and build the app and PKG:
tools/ps4/build.sh
```

The script reads the current build number from `BUILD_NUMBER`; set `PSBOX_BUILD_NUMBER` only when intentionally overriding it for a one-off build.

The script uses `build-ps4-homebrew/` as its incremental build directory. Override input and build paths with the environment variables in the prerequisites guide. A successful package build confirms compilation and packaging only; it does not establish that the app runs correctly on a PS4. The build script does not deploy to a console or publish a release.

The public source check is separate from compiling the PS4 app:

```sh
python3 tools/release/check-public-tree.py
```

## GitHub-hosted compilation

Manually run **Compile PS4 beta** (`compile-ps4.yml`) on the reviewed branch. It uses Ubuntu 24.04 and LLVM 21, verifies the public source and the private input archive checksum, then uploads only the PKG, SHA256SUMS and build identity as a 14-day Actions artifact. It does not deploy to a console or attach packages to releases automatically.

The maintainer must set `PSBOX_BUILD_INPUTS_URL` as an Actions secret containing a fresh, temporary download URL for the private build-input archive, and `PSBOX_BUILD_INPUTS_SHA256` as an Actions variable. Renew the URL before dispatching a later build. Never commit that URL, SDK archives, credentials or console logs. External inputs stay in separate private storage even if this source repository becomes public. Binary distribution still requires the review in [docs/LICENSE-STATUS.md](docs/LICENSE-STATUS.md).
