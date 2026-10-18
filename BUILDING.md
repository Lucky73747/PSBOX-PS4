# Building PSBOX for PS4

PSBOX's app version is recorded in `VERSION` and its source build number in `BUILD_NUMBER`. This source identifies build 107 as app version 0.1.0. The PS4 package's system version is kept independently increasing so the package can update earlier test installs.

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
