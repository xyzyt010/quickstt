# Distribution

## Supported Distribution Paths

QuickSTT is designed to support:
- local manual folder copy
- portable bootstrap install/update
- LAN package delivery
- configured WAN delivery over public IPv4 or IPv6
- GitHub release downloads
- release manifests and checksums for published artifacts

## Network Strategy

### Discovery

UDP is used for:
- fast LAN server discovery
- low-latency local availability checks

UDP is **not** the primary file-transfer protocol.

### File Transfer

HTTP/TCP is used for:
- manifests
- package downloads
- individual file fallback downloads

This is deliberate:
- it is more reliable than raw UDP file transfer
- it is easier to secure and reason about
- it performs better in real-world WAN conditions

## Preferred Address Order

The bootstrapper is designed to prefer:

1. LAN IPv4
2. LAN IPv6
3. WAN IPv4
4. WAN IPv6

This matches the most practical deployment reality:
- LAN should be fastest
- public IPv4 is often the most broadly reachable remote option
- public IPv6 works well when both sides truly have working global IPv6

## CGNAT Note

For public IPv4 distribution:
- the **server** needs reachable public IPv4 or equivalent forwarding
- the **client** does not need public IPv4 and can still download from behind CGNAT

For public IPv6 distribution:
- both sides need working global IPv6 reachability

## Manual Published WAN URLs

To explicitly publish public endpoints:

- create `published_server_urls.txt`
- put one public URL or host per line
- distribute the generated `server_urls.txt` alongside `QuickSTT_Portable.exe`
- if no real file is present, the build ships `published_server_urls.example.txt` as a template

This is the recommended way to support explicit WAN installs without relying only on local discovery.

## Release Artifacts

The Windows packaging flow now produces:

- `QuickSTT_Portable.exe`
  - bootstrap installer/updater
- `QuickSTT_Server/QuickSTT_LAN_Package.tar`
  - high-speed LAN package for one-shot installs
- `QuickSTT_DirectDownload/QuickSTT_Basic/`
  - lean standalone folder
  - optional services can be added later from the bundled server payload
- `QuickSTT_DirectDownload/QuickSTT_Full/`
  - full standalone folder for manual sharing or extracted installs
  - SmartHome lights and Android TV optional services are pre-expanded here
- `QuickSTT_DirectDownload/release_manifest.json`
  - machine-readable artifact metadata
- `QuickSTT_DirectDownload/SHA256SUMS.txt`
  - SHA-256 checksums for release verification

The server payload also includes optional service packages under:

- `QuickSTT_Server/files/addons/services/smart_life_enable.zip`
- `QuickSTT_Server/files/addons/services/android_tv_remote_runtime.zip`

This lets the basic app stay lean while still allowing later install-on-demand from the dashboard.

The standalone and server payloads are intentionally built from clean output folders so stale files from previous builds do not leak into public releases.

## Release Recommendations

Use GitHub Releases for public distribution:
- `QuickSTT_Portable.exe`
- optional standalone archive
- checksums
- release notes

Avoid committing large packaged outputs directly into git history.
