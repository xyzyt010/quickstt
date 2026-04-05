# Distribution

## Supported Distribution Paths

QuickSTT is designed to support:
- local manual folder copy
- portable bootstrap install/update
- LAN package delivery
- configured WAN delivery over public IPv4 or IPv6
- GitHub release downloads

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

This is the recommended way to support explicit WAN installs without relying only on local discovery.

## Release Recommendations

Use GitHub Releases for public distribution:
- `QuickSTT_Portable.exe`
- optional standalone archive
- checksums
- release notes

Avoid committing large packaged outputs directly into git history.

