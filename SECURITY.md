# Security Policy

## Supported versions

| Version | Supported |
|---|---|
| `main` + latest `v*` tag (e.g. `v2.0.0-alpha.1`) | ✅ |
| Older tags | ❌ — please update |

## Reporting a vulnerability — private first

**Do not open a public issue for security bugs.**

Email: `security@quickstt.com` (or use GitHub **Security → Report a vulnerability**).

Include:

- affected version / commit
- OS and arch (`Windows 10/11 amd64`, `Linux Mint 21/22 amd64`, `Ubuntu 22.04 amd64`, `aarch64` cross case)
- precise reproduction (mic access, tray, settings, updater, network)
- whether credentials or local files are involved
- whether scope is local-only, LAN, or internet-facing
- `RUST_LOG=info` logs if relevant

We aim to acknowledge within **72h** and to coordinate a fix + disclosure.

## What QuickSTT handles

- Local microphone (`cpal` / `PortAudio`) — no audio leaves the device unless you add a cloud backend
- Local text injection (`SendInput` on Windows, `wtype`/`xdotool` on Linux)
- **Secrets**: `windows_secret_store.cpp` DPAPI on Windows; `~/.config/QuickSTT/config.toml` on Linux (extend with `libsecret` if you store tokens)
- **Distribution**: `BuildApp.bat` Portable bootstrapper, `cargo deb` `.deb`, `linuxdeploy` AppImage, `QuickSTT_Server` LAN/WAN over HTTP/TCP

High-priority classes:

- credential leakage (API keys, `server_config.json`)
- unsigned or untrusted `QuickSTT_Portable.exe` / `.deb` / `AppImage` execution
- arbitrary file overwrite during update/install (`release_manifest.json` handling)
- remote code execution via package download
- unsafe SmartHome control (Tuya / Android TV) when those optional services are enabled

## Hardening

- Keep `server_urls.txt` + `published_server_urls.txt` out of git (see `.gitignore`).
- Enable firewall explicitly on server machines (`EnableQuickSTT_Server_Firewall.bat` on Windows, `ufw` on Linux).
- Verify every Release via `SHA256SUMS` (`sha256sum -c dist/SHA256SUMS`).
- Download only from `https://github.com/quickstt/quickstt/releases` or your own build from `main`.
- Future: signed Release assets + `cosign` for `.deb`/`.AppImage`.

## Disclosure

We will credit reporters in release notes if you wish, unless you prefer to remain anonymous.
