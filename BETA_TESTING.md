# Testing development builds

A PS4 build is not validated merely because compilation and packaging succeed. Record the exact source manifest/commit, package version and SHA-256, firmware, setup, test steps and observed result.

Before distributing any PKG, complete the binary checklist in [docs/LICENSE-STATUS.md](docs/LICENSE-STATUS.md), including corresponding source and third-party notices. This source preparation does not authorize or perform a release.

Never share `account.json`, refresh tokens, passwords, login codes or raw session dumps. Review logs for account identifiers, local addresses, ICE candidates and session information before sharing a redacted excerpt. Report a crash separately from an ordinary exit and distinguish decoded frames from displayed frames.
