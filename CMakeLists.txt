
cmake_minimum_required(VERSION 3.16)

project(QuickSTT VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 COMPONENTS Core Gui Widgets Svg Network Multimedia REQUIRED)

# Main Application
add_executable(QuickSTT_App
    Source/main.cpp
    Source/ahk_bridge.cpp
    Source/ahk_bridge.h
    Source/cloud_stt_manager.cpp
    Source/cloud_stt_manager.h
    Source/local_model_manager.cpp
    Source/local_model_manager.h
    Source/local_model_support.cpp
    Source/local_model_support.h
    Source/local_transcription_manager.cpp
    Source/local_transcription_manager.h
    Source/pill_widget.cpp
    Source/pill_widget.h
    Source/mainwindow.cpp
    Source/mainwindow.h
    Source/setup_wizard.cpp
    Source/setup_wizard.h
    Source/smart_life_manager.cpp
    Source/smart_life_manager.h
    Source/startup_utils.cpp
    Source/startup_utils.h
    Source/text_board.cpp
    Source/text_board.h
    Source/windows_secret_store.cpp
    Source/windows_secret_store.h
    resources_app.res
)
target_link_libraries(QuickSTT_App PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg Qt6::Network dxgi crypt32)
set_target_properties(QuickSTT_App PROPERTIES WIN32_EXECUTABLE ON)

# C++ Server Replacement Module
add_executable(QuickSTT_Server_App Source/qt_server_app.cpp resources_server.res)
target_link_libraries(QuickSTT_Server_App PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)
set_target_properties(QuickSTT_Server_App PROPERTIES WIN32_EXECUTABLE ON)

# Lightweight Loader (no Qt dependency)
add_executable(QuickSTT Source/loader.cpp)
target_link_libraries(QuickSTT PRIVATE wininet comctl32 ws2_32 iphlpapi)
# Static linkage for MinGW runtime
target_link_options(QuickSTT PRIVATE -static-libgcc -static-libstdc++ -static)
# Set entry point for WinMain
set_target_properties(QuickSTT PROPERTIES WIN32_EXECUTABLE ON)

# ── Native STT Service (replaces Python stt_service.exe) ──
# Zero Python / Qt dependency. Uses libvosk.dll + tensorflowlite_c.dll (dynamic load)
add_executable(stt_service_native
    Source/native/stt_service_native.cpp
)
target_include_directories(stt_service_native PRIVATE Source/native)
target_link_libraries(stt_service_native PRIVATE
    ${CMAKE_SOURCE_DIR}/Source/native/libportaudio.dll.a
    winmm
)
# Static link C++ runtime so it runs standalone
target_link_options(stt_service_native PRIVATE -static-libgcc -static-libstdc++ -static)
set_target_properties(stt_service_native PROPERTIES
    OUTPUT_NAME "stt_service"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

# Copy Resources (SVGs, DLLs, PortAudio) to Build Directory
file(GLOB RESOURCE_FILES "${CMAKE_SOURCE_DIR}/Source/*.svg" "${CMAKE_SOURCE_DIR}/Source/*.dll")
add_custom_command(TARGET QuickSTT_App POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${RESOURCE_FILES}
    $<TARGET_FILE_DIR:QuickSTT_App>
)

