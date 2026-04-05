QuickSTT Distribution Modes
===========================

1. QuickSTT_Portable.exe
   - Small bootstrap installer/updater.
   - Uses UDP on port 5001 only to discover a local QuickSTT server quickly.
   - Uses HTTP/TCP on port 5000 to download files after discovery.
   - Good for LAN installs and updates.

2. QuickSTT_Full.zip
   - Optional website-friendly ZIP package.
   - Built only when QUICKSTT_BUILD_DIRECT_ZIP=1 is set.
   - Extract it anywhere and run QuickSTT_App.exe.
   - No QuickSTT server is required for this mode.

3. QuickSTT_Full folder
   - Same as the ZIP, already extracted for manual copy/install.

4. QuickSTT_LAN_Package.tar
   - Fast uncompressed LAN package used by the bootstrapper.
   - Optimized for local network transfer speed instead of ZIP compression.

Firewall Notes
--------------

- Client machines normally do NOT need inbound firewall rules just to download.
- The server machine may need inbound allow rules for:
  - TCP 5000
  - UDP 5001
- Use EnableQuickSTT_Server_Firewall.bat on the server machine if needed.

IPv6 Notes
----------

- The server can advertise IPv4 and IPv6 addresses.
- Global IPv6 works only if the network, ISP/router, and Windows firewall all allow reachability.

Published WAN URLs
------------------

- Put public IPv4, public IPv6, or hostnames in `published_server_urls.txt`.
- One URL or host per line is supported.
- These URLs are copied into the distributed bootstrap package as `server_urls.txt`.
- Loader priority is: LAN IPv4, then LAN IPv6, then WAN IPv4, then WAN IPv6.
