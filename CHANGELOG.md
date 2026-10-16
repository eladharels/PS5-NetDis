# Changelog

All notable changes to NetDis. Versions follow [semantic versioning](https://semver.org):
the first number goes up for changes that break your setup, the second for new
features, the third for fixes. Each version is a
[release](https://github.com/eladharels/ps5-netdis/releases) with `NetDis.elf`
and a zip.

## [1.0.0] - 2026-10-16

First public release.

### Added
- Version number in the notification ("NetDis v1.0.0 running"), the log, and
  Settings > About.
- Releases with `NetDis.elf` and a zip, built and published by GitHub
  Actions when a new version reaches `main`.
- README with screenshots, how it works, troubleshooting and building.

### Changed
- NetDis has its own repository (it started in `ps5-payload`, now
  [ps5-60fps](https://github.com/eladharels/ps5-60fps)).
- Built with a fixed PS5 payload SDK version (v0.44) instead of whatever is
  newest, so every release can be rebuilt the same way.

## [0.1.0] - 2026-10-15

Not released as a download.

### Added
- First version: every TCP and UDP socket on the console with its owning
  process and game title ID, TCP state, and whether the network or only the
  console can reach it.
- **Ports** tab (listening, connections, all sockets), **Processes** tab,
  **Logs** and **Settings** tabs.
- **NetDis** tile under Media on the home screen; sending a newer
  `NetDis.elf` replaces the running copy; Payload Manager can stop it.

### Fixed (before the first version shipped)
- One idle connection froze the page for everyone.
- Unbound UDP sockets were counted as listening.
