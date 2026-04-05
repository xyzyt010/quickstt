# Releasing QuickSTT

## Recommended Public Release Model

Use the repository for:
- source code
- documentation
- issue tracking
- contribution workflow

Use GitHub Releases for:
- `QuickSTT_Portable.exe`
- standalone folder archives
- checksums
- release notes

## Suggested Release Assets

- `QuickSTT_Portable.exe`
- `QuickSTT_Full.zip` when a full website-download archive is needed
- SHA256 checksum file
- short release notes

## Suggested Release Process

1. Build with the normal packaging flow
2. Verify the widget, dashboard, and server app boot correctly
3. Generate release assets
4. Upload binaries to GitHub Releases
5. Publish checksums and notes
6. Announce compatibility notes and known limits

## Why Releases Instead of Git History for Binaries

Large binaries in git history:
- bloat the repository
- slow clones and fetches
- make source collaboration worse

GitHub Releases are the better place for packaged outputs.

