# QuickSTT Project — Continuation Command

## How to Continue the Rust Migration

When starting a new chat with an agent, provide this file as context.

---

## Step 1: Read the Migration Guide

```
Read the file: quickstt-rust/MIGRATION_GUIDE.md
```

This file contains:
- Complete architecture overview
- All 20 migration steps with status checkboxes
- C++ source file → Rust module mapping
- IPC protocol specification
- Windows Registry settings reference
- Build commands and dependencies

## Step 2: Check Current Progress

Read MIGRATION_GUIDE.md and find the first step with `[ ] NOT STARTED` or `[x] PARTIALLY DONE`.

Current known state:
- Step 1: [x] PARTIALLY DONE — protocol types done, stt_service.rs skeleton has import errors
- Step 2-20: [ ] NOT STARTED

## Step 3: Fix Immediate Issues

The file `quickstt-rust/quickstt-core/src/stt_service.rs` has these known bugs:
1. Line 3: `std::path::Buf` should be `std::path::PathBuf`
2. Line 1: Missing `use anyhow::Context;` import

## Step 4: Continue Implementation

Work through steps in order. Each step must compile before moving to the next.

Build command:
```
cd quickstt-rust && cargo build --workspace
```

---

## Project Root

```
C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app
```

## Key Files

| File | Purpose |
|---|---|
| `quickstt-rust/MIGRATION_GUIDE.md` | Complete migration guide (READ FIRST) |
| `quickstt-rust/Cargo.toml` | Workspace root |
| `quickstt-rust/quickstt-ipc/src/protocol.rs` | IPC protocol types (DONE) |
| `quickstt-rust/quickstt-core/src/stt_service.rs` | STT service manager (NEEDS FIX) |
| `quickstt-rust/quickstt-gui/src/main.rs` | GUI entry point (EMPTY) |
| `Source/CMakeLists.txt` | C++ build system (reference only) |
| `DEADCODE_AUDIT.md` | Dead code audit report |

---

## Critical Rules

1. DO NOT modify `Source/native/stt_service_native.cpp`
2. DO NOT modify the pipe protocol format
3. Registry format must match Qt's QSettings output
4. All file paths use `\` on Windows
5. Use `anyhow` for error propagation, `thiserror` for custom types
6. Use `tokio` for all I/O
7. Always use Win32 `W` (Unicode) API variants
8. Win32 UI calls must be on the main thread
