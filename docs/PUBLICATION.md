# Source publication checklist

The supported source review is a fresh export of the selected files:

```sh
python3 tools/release/check-public-tree.py
python3 tools/release/export-public-source.py --output ../PSBOX-PS4-source
```

The export contains the selected application sources, dependency notices and a manifest. It excludes Git history, generated packages, external SDK inputs, console data and files outside the public allowlist. It never contacts a hosting service or publishes automatically.

Before changing repository visibility, review the exact export, all branches and tags, commit history, releases, Actions artifacts and license obligations. The export checks file selection, documentation links, notices, symlinks and common credential patterns. It cannot identify every secret, verify runtime behavior, clear third-party licensing, or make other repository refs safe to publish.

The public `build-ps4.yml` workflow performs source checks only. It does not compile or publish a PKG. Complete [LICENSE-STATUS.md](LICENSE-STATUS.md) before distributing a binary.
