# Agent rules (QuickSTT repo)

## 1. The Windows dev host is the USER'S WORKING MACHINE.
- **Launching the new app on Windows is allowed** (the user approved it);
  **never test or verify behavior there** — no clicks, no hovers, no drag,
  no screenshots, no xdotool. Process/launch sanity (exists, version)
  only, then stop touching it.
- **All verification happens on the Mint box via SSH**: `hs01@192.168.1.12`,
  key `~/.ssh/hs01_windows_key`, deploy to `~/quickstt` from the verified
  GitHub release tarball. Vision/state verification via `xwininfo`/`xdotool`
  geometry + app logs only (no xdotool click/hover UI tests) — the user
  does interactive testing.

## 2. Release pipeline (autonomous, no questions).
- Bump `2.0.0-alpha.N` in `quickstt-rust/Cargo.toml`, `Cargo.lock`,
  `quickstt-rust/installer/quickstt.iss`, `CHANGELOG.md`; commit, tag
  `desktop-v2.0.0-alpha.N`, push. Never leave a release half-fixed:
  force-move the tag if a commit lands after tagging before publish.
- Build installer locally with `Build-Installer.ps1 -NoBuild`, upload it,
  regenerate `SHA256SUMS` including the installer, update release notes,
  verify hashes, redeploy Mint from the release tarball.
