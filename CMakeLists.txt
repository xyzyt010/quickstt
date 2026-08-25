
cmake_minimum_required(VERSION 3.16)

project(QuickSTT VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(WIN32)
find_package(Qt6 COMPONENTS Core Gui Widgets Svg Network Multimedia REQUIRED)
add_subdirectory(third_party/QtAwesome EXCLUDE_FROM_ALL)
else()
find_package(Qt6 COMPONENTS Core Gui Widgets Svg Network Multimedia QUIET)
if(Qt6_FOUND)
  add_subdirectory(third_party/QtAwesome EXCLUDE_FROM_ALL)
else()
  message(STATUS "Qt6 not found on Linux — C++ GUI targets disabled, use cargo build -p quickstt-gui")
endif()
endif()

# Main Application (Legacy C++ GUI) — cross-platform: Windows native + Linux (Qt6 widgets)
set(QUICKSTT_APP_SOURCES
    Source/main.cpp
    Source/cloud_stt_manager.cpp
    Source/cloud_stt_manager.h
    Source/android_tv_manager.cpp
    Source/android_tv_manager.h
    Source/local_model_manager.cpp
    Source/local_model_manager.h
    Source/local_frontend_stt_manager.cpp
    Source/local_frontend_stt_manager.h
    Source/local_model_support.cpp
    Source/local_model_support.h
    Source/optional_service_manager.cpp
    Source/optional_service_manager.h
    Source/optional_service_support.cpp
    Source/optional_service_support.h
    Source/pill_widget.cpp
    Source/pill_widget.h
    Source/mainwindow.cpp
    Source/mainwindow.h
    Source/setup_wizard.cpp
    Source/setup_wizard.h
    Source/smart_life_manager.cpp
    Source/smart_life_manager.h
    Source/home_assistant_manager.cpp
    Source/home_assistant_manager.h
    Source/startup_utils.cpp
    Source/startup_utils.h
    Source/text_board.cpp
    Source/text_board.h
)
if(WIN32)
list(APPEND QUICKSTT_APP_SOURCES
    Source/ahk_bridge.cpp
    Source/ahk_bridge.h
    Source/windows_secret_store.cpp
    Source/windows_secret_store.h
    resources_app.rc
)
endif()

if(WIN32 OR Qt6_FOUND)
add_executable(QuickSTT_App ${QUICKSTT_APP_SOURCES})
target_link_libraries(QuickSTT_App PRIVATE QtAwesome Qt6::Core Qt6::Widgets Qt6::Svg Qt6::Network)
if(WIN32)
target_link_libraries(QuickSTT_App PRIVATE dxgi crypt32)
set_target_properties(QuickSTT_App PROPERTIES WIN32_EXECUTABLE ON)
else()
set_target_properties(QuickSTT_App PROPERTIES OUTPUT_NAME "quickstt-app")
endif()
endif()

# Make the development build runnable when started directly from Explorer.
# Qt is dynamically linked, so a bare .exe otherwise fails at startup with a
# missing Qt6Core.dll error after the shell environment has been reset.
if(WIN32)
    get_filename_component(QUICKSTT_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    set(QUICKSTT_WINDEPLOYQT "${QUICKSTT_QT_BIN_DIR}/windeployqt.exe")
    if(EXISTS "${QUICKSTT_WINDEPLOYQT}")
        add_custom_command(TARGET QuickSTT_App POST_BUILD
            COMMAND "${QUICKSTT_WINDEPLOYQT}"
                --svg --network --no-translations --no-opengl-sw --no-compiler-runtime
                --dir "$<TARGET_FILE_DIR:QuickSTT_App>"
                "$<TARGET_FILE:QuickSTT_App>"
            COMMENT "Deploying Qt runtime for QuickSTT_App"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt was not found; QuickSTT_App will require Qt on PATH")
    endif()

    get_filename_component(QUICKSTT_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(QUICKSTT_RUNTIME_DLL libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if(EXISTS "${QUICKSTT_MINGW_BIN_DIR}/${QUICKSTT_RUNTIME_DLL}")
            add_custom_command(TARGET QuickSTT_App POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QUICKSTT_MINGW_BIN_DIR}/${QUICKSTT_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:QuickSTT_App>/${QUICKSTT_RUNTIME_DLL}"
                VERBATIM
            )
        endif()
    endforeach()
endif()

# C++ Server Replacement Module (Legacy - Disabled for Rust Migration)
# add_executable(QuickSTT_Server_App Source/qt_server_app.cpp resources_server.rc)
# target_link_libraries(QuickSTT_Server_App PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network shell32)
# set_target_properties(QuickSTT_Server_App PROPERTIES WIN32_EXECUTABLE ON)

# Lightweight Loader (Windows-only)
if(WIN32)
add_executable(QuickSTT Source/loader.cpp resources_loader.rc)
target_link_libraries(QuickSTT PRIVATE wininet comctl32 ws2_32 iphlpapi)
target_link_options(QuickSTT PRIVATE -static-libgcc -static-libstdc++ -static)
set_target_properties(QuickSTT PROPERTIES WIN32_EXECUTABLE ON)
endif()

# ── Native STT Service (replaces Python stt_service.exe) ──
# Zero Python / Qt dependency. Uses libvosk.dll + tensorflowlite_c.dll (dynamic load)
set(RNNOISE_ROOT "${CMAKE_SOURCE_DIR}/third_party/audio_preprocess/rnnoise/rnnoise-0.2")
add_library(rnnoise_static STATIC
    "${RNNOISE_ROOT}/src/denoise.c"
    "${RNNOISE_ROOT}/src/rnn.c"
    "${RNNOISE_ROOT}/src/pitch.c"
    "${RNNOISE_ROOT}/src/kiss_fft.c"
    "${RNNOISE_ROOT}/src/celt_lpc.c"
    "${RNNOISE_ROOT}/src/nnet.c"
    "${RNNOISE_ROOT}/src/nnet_default.c"
    "${RNNOISE_ROOT}/src/parse_lpcnet_weights.c"
    "${RNNOISE_ROOT}/src/rnnoise_data.c"
    "${RNNOISE_ROOT}/src/rnnoise_tables.c"
)
target_include_directories(rnnoise_static PUBLIC
    "${RNNOISE_ROOT}/include"
    "${RNNOISE_ROOT}/src"
)
target_compile_definitions(rnnoise_static PRIVATE RNNOISE_BUILD)
set_target_properties(rnnoise_static PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(WIN32)
add_executable(stt_service_native
    Source/native/stt_service_native.cpp
    Source/native/audio_preprocess.cpp
    Source/native/audio_preprocess.h
)
target_include_directories(stt_service_native PRIVATE Source/native)
target_link_libraries(stt_service_native PRIVATE rnnoise_static ${CMAKE_SOURCE_DIR}/Source/native/libportaudio.dll.a winmm)
target_compile_definitions(stt_service_native PRIVATE QUICKSTT_USE_STATIC_RNNOISE)
target_link_options(stt_service_native PRIVATE -static-libgcc -static-libstdc++ -static)
set_target_properties(stt_service_native PROPERTIES OUTPUT_NAME "stt_service" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
else()
# Linux native service — uses system PortAudio + dl + pthread via platform.h (libvosk.so / libonnxruntime.so / libtensorflowlite_c.so)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
add_executable(stt_service_native
    Source/native/stt_service_native.cpp
    Source/native/audio_preprocess.cpp
    Source/native/audio_preprocess.h
)
target_include_directories(stt_service_native PRIVATE Source/native ${PORTAUDIO_INCLUDE_DIRS})
target_link_directories(stt_service_native PRIVATE ${PORTAUDIO_LIBRARY_DIRS})
target_link_libraries(stt_service_native PRIVATE rnnoise_static ${PORTAUDIO_LIBRARIES} dl pthread)
target_compile_definitions(stt_service_native PRIVATE QUICKSTT_USE_STATIC_RNNOISE)
target_compile_options(stt_service_native PRIVATE ${PORTAUDIO_CFLAGS_OTHER})
set_target_properties(stt_service_native PROPERTIES OUTPUT_NAME "stt_service" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()

if(WIN32)
add_custom_command(TARGET stt_service_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:stt_service_native>/audio_preprocess/deepfilter" "$<TARGET_FILE_DIR:stt_service_native>/audio_preprocess/ten_vad"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/third_party/audio_preprocess/deepfilter/deep-filter.exe" "$<TARGET_FILE_DIR:stt_service_native>/audio_preprocess/deepfilter/deep-filter.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/third_party/audio_preprocess/deepfilter/DeepFilterNet3.tar.gz" "$<TARGET_FILE_DIR:stt_service_native>/audio_preprocess/deepfilter/DeepFilterNet3.tar.gz"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/third_party/audio_preprocess/ten_vad/ten_vad.dll" "$<TARGET_FILE_DIR:stt_service_native>/audio_preprocess/ten_vad/ten_vad.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/Source/native/tensorflowlite_c.dll" "$<TARGET_FILE_DIR:stt_service_native>/tensorflowlite_c.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/Source/native/libportaudio.dll" "$<TARGET_FILE_DIR:stt_service_native>/libportaudio.dll"
)
else()
add_custom_command(TARGET stt_service_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:stt_service_native>/audio_preprocess/deepfilter" "$<TARGET_FILE_DIR:stt_service_native>/audio_preprocess/ten_vad"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/third_party/audio_preprocess/deepfilter/DeepFilterNet3.tar.gz" "$<TARGET_FILE_DIR:stt_service_native>/audio_preprocess/deepfilter/DeepFilterNet3.tar.gz" || true
)
endif()

# Copy Resources (SVGs, DLLs, PortAudio) to Build Directory
if(TARGET QuickSTT_App AND WIN32)
find_program(CARGO_EXECUTABLE NAMES cargo)
if(CARGO_EXECUTABLE)
add_custom_target(quickstt_popup_rust
    COMMAND ${CARGO_EXECUTABLE} build --release --locked
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/quickstt_popup"
    BYPRODUCTS "${CMAKE_SOURCE_DIR}/quickstt_popup/target/release/quickstt_popup.exe"
    COMMENT "Building the QuickSTT Ctrl+Space popup"
    VERBATIM
)
add_dependencies(QuickSTT_App quickstt_popup_rust)
add_custom_command(TARGET QuickSTT_App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/quickstt_popup/target/release/quickstt_popup.exe"
        "$<TARGET_FILE_DIR:QuickSTT_App>/quickstt_popup.exe"
)
else()
message(WARNING "Cargo was not found; QuickSTT_App will be built without quickstt_popup.exe")
endif()

file(GLOB RESOURCE_FILES "${CMAKE_SOURCE_DIR}/Source/*.svg" "${CMAKE_SOURCE_DIR}/Source/*.dll")
add_custom_command(TARGET QuickSTT_App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${RESOURCE_FILES}
    $<TARGET_FILE_DIR:QuickSTT_App>
)

# Stage the native Rust engine used by the NVIDIA Parakeet model.
set(PARAKEET_ENGINE
    "${CMAKE_SOURCE_DIR}/parakeet_engine/target/release/parakeet_engine.exe")
if(EXISTS "${PARAKEET_ENGINE}")
add_custom_command(TARGET QuickSTT_App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:QuickSTT_App>/tools/parakeet"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PARAKEET_ENGINE}"
        "$<TARGET_FILE_DIR:QuickSTT_App>/tools/parakeet/parakeet_engine.exe"
)
else()
message(WARNING "Parakeet engine was not found; ONNX Parakeet local models will be unavailable")
endif()

# Nemotron 3.5 (Handy stack): prebuilt tools/nemotron/ runtime
# (nemotron_engine.exe + transcribe.dll + ggml*.dll + fetch_and_convert.py).
# Same model/runtime as Handy: handy-computer GGUF + transcribe.cpp.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/nemotron/nemotron_engine.exe")
    add_custom_command(TARGET QuickSTT_App POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:QuickSTT_App>/tools/nemotron"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/tools/nemotron"
            "$<TARGET_FILE_DIR:QuickSTT_App>/tools/nemotron"
        COMMENT "Staging Handy-compatible Nemotron/transcribe.cpp runtime"
    )
endif()

# Copy the optional OpenVINO inference helper when it is present.  Native
# Parakeet-only builds intentionally do not include this legacy script.
if(EXISTS "${CMAKE_SOURCE_DIR}/Source/openvino_stt_infer.py")
add_custom_command(TARGET QuickSTT_App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/Source/openvino_stt_infer.py"
    $<TARGET_FILE_DIR:QuickSTT_App>
)
endif()

# Mirror latest build artifacts into the QuickSTT_App distribution directory
# (Windows dev convenience only — Linux release binaries are staged by CI)
if(WIN32)
add_custom_command(TARGET QuickSTT_App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/QuickSTT_App"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:QuickSTT_App>/QuickSTT_App.exe" "${CMAKE_SOURCE_DIR}/QuickSTT_App/QuickSTT_App.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:QuickSTT_App>/stt_service.exe" "${CMAKE_SOURCE_DIR}/QuickSTT_App/stt_service.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:QuickSTT_App>/quickstt_popup.exe" "${CMAKE_SOURCE_DIR}/QuickSTT_App/quickstt_popup.exe"
    COMMENT "Deploying latest binaries to QuickSTT_App distribution folder"
)
endif()
endif()
