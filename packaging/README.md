# QuickSTT packaging recipes

Canonical release assets (tag `desktop-v1.0.0`, repo `xyzyt010/quickstt`):

| Asset | Platform |
|---|---|
| `quickstt_1.0.0-1_amd64.deb` | Debian / Ubuntu / Linux Mint, x86_64 (built in CI with `cargo deb`) |
| `quickstt-desktop-v1.0.0-x86_64.tar.gz` | Linux x86_64, any distro (universal tarball) |
| `quickstt-desktop-v1.0.0-aarch64.tar.gz` | Linux aarch64, any distro (universal tarball) |
| `QuickSTT-Setup-1.0.0.exe` | Windows x64 Inno Setup installer (runs on Windows ARM64 via x64 emulation) |
| `quickstt-desktop-v1.0.0-win64-portable.zip` | Windows x64 portable (no install) |
| `quickstt-android-release.apk` | Android arm64-v8a (separate release, tag `v1.0.0`) |
| `SHA256SUMS` | Checksums for the assets of that release |

## Why there is no .rpm / .pkg.tar in CI

CI publishes the `.deb` and the arch-explicit universal tarballs only.
Building an RPM or an Arch package in CI would need a second toolchain
(`rpmbuild` / `makepkg`) and distro-specific runners for what is, for a
prebuilt binary, a pure re-pack of the tarball — high maintenance, low
value. Instead:

- `install.sh` (repo root) covers Fedora, Arch, Gentoo and openSUSE
  directly from the tarball, user-local, no sudo.
- The recipes in this directory let users (or distro packagers) build a
  native package from the same tarball on the target distro, which also
  gets dependency names exactly right for that distro.

## Recipes here (all version-templated at 1.0.0)

| File | Distro | Build |
|---|---|---|
| `PKGBUILD` | Arch / Manjaro / EndeavourOS | `makepkg -si` (in a directory containing only `PKGBUILD`) |
| `quickstt.spec` | Fedora / RHEL / Rocky / Alma | `rpmbuild -bb quickstt.spec` (tarball in `~/rpmbuild/SOURCES/`) |
| `quickstt.ebuild` | Gentoo | copy to `quickstt-1.0.0.ebuild` in a local overlay under `app-accessibility/quickstt/`, then `emerge -av quickstt` |

All three install the payload to `/opt/quickstt`, symlink
`/usr/bin/quickstt`, and install the desktop entry + icon. To bump the
version, change `pkgver` / `Version:` / the ebuild file name (and
`SRC_URI`) from `1.0.0` to the new version and refresh checksums
(`makepkg -g`, `sha256sum`, or `ebuild ... manifest`).
