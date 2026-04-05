# QuickSTT

QuickSTT is a Windows-first voice typing, wakeword, automation, and local/cloud speech platform built around a native C++/Qt desktop app, a native speech service, and an optional LAN/WAN distribution server.

The project is designed for:
- precise voice typing into any focused Windows app
- wakeword-first interaction with low-friction desktop control
- local and cloud speech-to-text backends
- LAN and remote distribution/update workflows
- future expansion to macOS and Linux

## What QuickSTT Does

QuickSTT combines several layers into one end-user product:
- a floating desktop widget for live voice control and dictation
- a dashboard for models, wakewords, cloud providers, GPU selection, Smart Life/Tuya integration, and settings
- a native background speech service for wakeword, listening state, audio capture, and STT routing
- a server app for shipping updates and full packages across LAN or configured WAN paths
- a portable bootstrapper that can discover a local server and install or update the app

## Major Features

- Wakeword support with native and bundled paths including OpenWakeWord and Porcupine-oriented workflows
- Local STT with Vosk and optional Whisper-family runtimes
- Cloud STT provider support for OpenAI, Google Speech-to-Text, ElevenLabs, AssemblyAI, Sarvam, and Reverie
- Local model cataloging for CPU and GPU-targeted variants, including NVIDIA, Intel OpenVINO, and expandable runtime packaging
- Floating voice widget with waveform UI, model selection, transcript panel, and command handling
- Text injection and special key commands on Windows through native input paths and AutoHotkey-assisted flows
- Smart Life / Tuya control integration from the dashboard and spoken commands
- Server-based update and install distribution
- Direct-download fallback distribution with a full standalone folder package
- Portable bootstrapper distribution for website and shared-link installs

## Architecture

QuickSTT is currently organized around four production pieces:

1. `QuickSTT.exe`
   - Lightweight bootstrapper / installer / updater
   - Discovers a local server via UDP and downloads packages over HTTP/TCP

2. `QuickSTT_App.exe`
   - Main Qt desktop application
   - Hosts the widget, dashboard, model management UI, cloud integrations, and automation hooks

3. `stt_service.exe`
   - Native speech service
   - Handles wakeword, audio capture, and STT orchestration

4. `QuickSTT_Server_App.exe`
   - Qt-based distribution server
   - Serves manifests, package payloads, and manual distribution assets over LAN/WAN

See [Architecture](./docs/ARCHITECTURE.md) for a deeper breakdown.

## Distribution Modes

QuickSTT supports three practical delivery modes:

- `QuickSTT_Portable.exe`
  - Small bootstrapper
  - Best for installer/updater flow
- `QuickSTT_DirectDownload/QuickSTT_Full`
  - Full standalone folder
  - Best for manual copy, shared drive, or extracted deployment
- GitHub Releases
  - Recommended public distribution channel for most users
  - Best for source + binaries without committing release artifacts into git history

See [Distribution](./docs/DISTRIBUTION.md) for the network and packaging model.

## Repository Layout

- `Source/`
  - Main application, widget UI, managers, server app, loader, and native speech service sources
- `Source/native/`
  - Native speech-service sources and low-level integration pieces
- `third_party/`
  - Bundled runtime/helper dependencies used by builds and packaging
- `docs/`
  - Architecture, distribution, and roadmap documentation
- `BuildApp.bat`
  - Main Windows packaging/build script
- `package_optional_local_runtimes.ps1`
  - Optional runtime/model packaging pipeline

## Build and Package

Windows is the primary supported development environment today.

Typical packaging flow:

```powershell
./BuildApp.bat
```

Primary output families:
- `QuickSTT_App/`
- `QuickSTT_Server/`
- `QuickSTT_DirectDownload/`
- `QuickSTT_Portable.exe`

Optional website ZIP packaging is intentionally off by default because it is large and slow to create. Enable it only for release builds.

## Security Notes

- Windows secrets are stored using DPAPI-backed handling where available
- Cloud credentials should never be committed to source control
- Release binaries should be distributed through signed or checksummed release assets
- Firewall rules should be explicitly opt-in on server machines

See [Security Policy](./SECURITY.md) for disclosure guidance and hardening expectations.

## Open Source License

QuickSTT is released under the [MIT License](./LICENSE).

That means:
- commercial use is allowed
- modification is allowed
- distribution is allowed
- private use is allowed

## Contribution and Support

QuickSTT is actively looking for contributors interested in:
- speech systems and STT runtime integration
- GPU acceleration and model packaging
- update/distribution architecture
- UI/UX improvements
- security hardening
- macOS support
- Linux desktop support

Start with:
- [Contributing](./CONTRIBUTING.md)
- [Roadmap](./docs/ROADMAP.md)

## Platform Ambition

Current priority order:

1. Windows polish and release stability
2. macOS support
3. Linux desktop distributions
4. additional local runtime backends
5. stronger release automation and signing

## Recommended Public Release Strategy

For a professional public project:
- keep source code in git
- keep packaged binaries in GitHub Releases
- publish `QuickSTT_Portable.exe` and the standalone package as release assets
- avoid committing large packaged folders and archives into repository history

