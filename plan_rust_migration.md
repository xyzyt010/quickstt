# QuickSTT Rust Migration Plan

## 📖 Overview

This document serves as the master plan and agent coordination file for migrating the QuickSTT application from C++ to a modern Rust-based architecture. 

**Architectural Directives:**
1. **To be converted to Rust:** The GUI, Core Logic, Orchestration, Command Routing, and Event Management.
2. **To be kept in C++/Original Language (via FFI):** Active Wakeword Detection, Model Backends, and Frameworks (e.g., Native STT inference).
3. **To be explicitly excluded/removed from the Rust application:** Smart Home logic, Tuya Pairing, Light Control Panel, Android TV Controllers, Cloud Models (OpenAI, etc.), and Cloud Provider APIs. 
   - *Note:* The C++ source files for these excluded components must be **kept** in the repository structure as legacy code, but they should be disconnected from the new Rust orchestration and build pipeline.

---

## 🤖 Agent Workflow & Instructions

### How to use this document:
- **Executing a Phase:** Read the phase objectives carefully. Follow the "Agent Step-by-Step" instructions. Ensure your code compiles and integrates correctly before considering the phase complete.
- **Ticking Off Phases:** When a phase is successfully completed, change the checkbox from `[ ]` to `[x]`. 
- **Updating the Plan:** If you encounter technical roadblocks, design flaws, or realize a different approach is necessary, you are expected to update this document. 
  - Add a section titled `### Updates / Modifications` beneath the respective phase.
  - Write a professional, succinct explanation of the architectural change and why it was required.
- **Status Reports:** At the end of your session, ensure this document accurately reflects the current state of the codebase.

---

## 🚀 Migration Phases

### [x] Phase 1: Rust Workspace Initialization & Source Scoping
**Objective:** Establish the foundational Rust project structure and isolate the legacy components that are being deprecated.

**Agent Step-by-Step:**
1. Initialize the root Rust project using `cargo init quickstt-rust` (or an appropriate workspace setup if splitting into crates).
2. Configure `.gitignore` to prevent committing Cargo artifacts.
3. Review the existing `CMakeLists.txt` and C++ source tree. Identify the files associated with the Smart Home, Android TV, and Cloud API features.
4. Modify the legacy build system (or create a new CMake config for the FFI library) to compile ONLY the Wakeword and Local STT Native modules into a static/dynamic library (`.lib` / `.dll` / `.so`).
5. Ensure the deprecated C++ files remain in the repository but are excluded from the new Rust-linked build step.

---

### [x] Phase 2: Rust-Native ML Integration (Wakeword & STT)
**Objective:** Integrate the modern Rust-based machine learning stack, completely replacing the legacy C++ ML backends.

**Agent Step-by-Step:**
1. Update `Cargo.toml` in the Rust workspace to include `livekit-wakeword`, `whisper-rs`, and `tract-onnx` (for YAMNet).
2. Create an `ml` or `audio_processing` module in the Rust project.
3. Implement the `WakeWordModel` wrapper using `livekit-wakeword` to support multiple custom `.onnx` wakewords.
4. Implement the environmental sound detection (YAMNet-256) via pure Rust ONNX inference.
5. Set up the `whisper-rs` integration for local transcription, strictly enforcing the lazy-loading requirement to meet the 10 MB idle RAM constraint.

### Updates / Modifications
*Architecture Shift:* Initially, we planned to retain the legacy C++ Native STT Service (Vosk/RNNoise) and write FFI bindings. Per the finalized `architectur_rust_migration.md`, we are pivoting to a purely Rust-native and Rust-bound ML architecture (`livekit-wakeword` and `whisper-rs`). This eliminates the need for maintaining the legacy C++ FFI code, greatly simplifies the build process, and allows us to hit the < 5 MB idle RAM constraint via aggressive lazy loading.

---

### [x] Phase 3: Audio Capture & Pipeline Integration
**Objective:** Build the audio processing pipeline in Rust and connect it to the ML tasks using the dual-stream architecture.

**Agent Step-by-Step:**
1. Evaluate and integrate a Rust audio capture library (`cpal` and `dasp`).
2. Set up a dedicated, high-priority audio thread to capture microphone input with minimal latency.
3. Implement the Two-Stream Voice Pipeline Flow:
   - Stream 1 (Always-on): 16kHz mono low-bitrate stream feeding exactly 1280 sample chunks to the Wakeword thread.
   - Stream 2 (On-Demand): Full quality WASAPI stream lazy-loaded and routed into the Transcription engine upon wakeword activation.
4. Handle the asynchronous retrieval of recognized text from the STT engine back to the main Rust application.

---

### [x] Phase 4: Core Orchestration & Command Routing Translation
**Objective:** Re-implement the central nervous system of QuickSTT in Rust.

**Agent Step-by-Step:**
1. Implement the main application state machine and orchestration module.
2. Port the `queueControlCommand` logic and general event loop from C++ to Rust.
3. Re-wire the internal command routing. **Crucially:** Ensure that the routing completely ignores any Cloud STT, Smart Home, or Android TV endpoints.
4. Port the LAN UDP Discovery and HTTP transfer protocols if they are part of the core local experience (as per the Distribution Flow).
5. Add comprehensive logging (`tracing` or `log` crates) to replicate the legacy application's diagnostic output.

---

### [x] Phase 5: GUI Framework Setup & Application Shell
**Objective:** Prepare the Rust-based Graphical User Interface that will mimic the legacy application.

**Agent Step-by-Step:**
1. Select a Rust GUI framework capable of recreating the existing C++ UI exactly (e.g., `Slint`, `Iced`, or `cxx-qt` for Qt interoperability).
2. Initialize the main window (`MainWindow` equivalent).
3. Set up the inter-thread communication channels (e.g., `mpsc` or `tokio` channels) between the GUI thread and the background orchestration/audio threads.
4. Create the core visual layout, ensuring it matches the proportions and styling of the original QuickSTT application.

---

### [x] Phase 6: GUI Implementation & State Binding
**Objective:** Fully implement the visual interface and wire it to the real-time application state.

**Agent Step-by-Step:**
1. Re-implement all individual UI components, indicators, and text fields.
2. Create the cross-community bridges (e.g., the `setText()` equivalent) so that recognized text from Phase 3 flows seamlessly onto the screen.
3. Wire UI control buttons (start, stop, settings) to their respective functions in the Rust orchestrator.
4. Ensure any visual elements that previously tied to the Smart Home / TV / Cloud functionality are either removed entirely or kept purely cosmetic/disabled, depending on the exact UX parity required by the transition.

---

### [x] Phase 7: Build System Unification & Packaging
**Objective:** Integrate the Rust Cargo build with the required dependencies into a cohesive deployment package.

**Agent Step-by-Step:**
1. Update `BuildApp.bat` (and associated packaging scripts like PowerShell) to orchestrate both the CMake compilation (for the FFI ML library) and the Cargo build.
2. Implement a unified build script (e.g., a Cargo `build.rs`) that statically or dynamically links the compiled C++ ML library into the final Rust binary.
3. Ensure all necessary dynamic link libraries (`.dll` files, like TensorFlow/Vosk/Qt if applicable) and ML model folders are automatically copied into the final staging directory.
4. Validate that the application can be built cleanly from scratch using the new scripts.

### Updates / Modifications
*Build Architecture Shift:* Because we transitioned to pure Rust ONNX (`tract`) and `whisper-rs` in Phase 2, we no longer need to invoke CMake to build C++ FFI `.dll` files.
- We updated `Cargo.toml` with aggressive `opt-level = 3`, `lto = true`, and `panic = "abort"` to hit the 5 MB footprint constraint.
- We added a custom `build.rs` and `manifest.xml` to `quickstt-gui` using `winresource` to guarantee native DPI scaling on Windows, preventing blurry text on high-DPI monitors.

---

### [x] Phase 8: Quality Assurance, Profiling, & Cleanup
**Objective:** Ensure performance and clean up the transition.

**Agent Step-by-Step:**
1. Run thorough integration tests from voice activation to GUI response.
2. Profile the Rust/C++ FFI boundary for latency bottlenecks (N/A - FFI removed).
3. Verify that the final executable size and performance are acceptable (< 5MB idle target implemented).
4. Update `README.md` and documentation to reflect the new architecture.
