# Architecture

## Overview

QuickSTT is a Windows-first desktop system composed of:
- a bootstrapper
- a main UI application
- a native speech service
- a distribution server

## Components

### `QuickSTT.exe`

Responsibilities:
- discover local servers
- resolve install/update sources
- download package or manifest-based updates
- launch the installed app

### `QuickSTT_App.exe`

Responsibilities:
- widget UI
- dashboard UI
- settings persistence
- cloud STT integration
- local model selection and management
- Smart Life / Tuya control
- transcript handling and command routing

### `stt_service.exe`

Responsibilities:
- audio capture
- wakeword processing
- local STT orchestration
- front-end/backend listening state transitions

### `QuickSTT_Server_App.exe`

Responsibilities:
- serve manifests and packaged files
- answer LAN discovery probes
- host direct package payloads
- support manual and network-driven distribution

## Data Flow

Typical voice flow:

1. Wakeword or manual activation starts listening
2. Audio is captured by the speech service
3. Audio is routed to local or cloud STT
4. Transcript returns to the app
5. App either types text, triggers commands, or performs automation/device actions

Typical distribution flow:

1. Bootstrapper starts
2. UDP LAN discovery is attempted first
3. If a server is discovered, package or manifest transfer occurs over HTTP/TCP
4. If no LAN server is found, configured remote URLs are tried
5. Installed app launches

