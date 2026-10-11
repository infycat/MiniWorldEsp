cmake_minimum_required(VERSION 3.24)
project(mcp_esp_overlay VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "MCP ESP Overlay currently supports Windows only (Win32 + DirectX 11).")
endif()

enable_language(RC)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/mcp-esp-icon.ico"
               "${CMAKE_CURRENT_BINARY_DIR}/mcp-esp-icon.ico" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/app.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/app.rc" COPYONLY)

include(FetchContent)
include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_WIN32_EXECUTABLE ON)
# Use the static MSVC CRT as well so either supported compiler can produce a
# standalone release executable without requiring a separately installed VC++
# runtime package.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MCP_ESP_FETCH_DEPENDENCIES "Fetch Dear ImGui and nlohmann/json with CMake" ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

if(MCP_ESP_FETCH_DEPENDENCIES)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9b
        GIT_SHALLOW TRUE)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imgui nlohmann_json)
else()
    if(NOT DEFINED IMGUI_DIR OR NOT EXISTS "${IMGUI_DIR}/imgui.h")
        message(FATAL_ERROR "Set IMGUI_DIR to a Dear ImGui source checkout when MCP_ESP_FETCH_DEPENDENCIES=OFF")
    endif()
    find_package(nlohmann_json 3.11 REQUIRED)
endif()

if(MCP_ESP_FETCH_DEPENDENCIES)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")

add_executable(mcp_esp_overlay WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
    src/main.cpp
    src/esp/data_source.cpp
    src/esp/diagnostics.cpp
    src/esp/live_process_source.cpp
    src/esp/projection.cpp
    src/esp/draw_esp.cpp
    src/esp/rigs.cpp
    src/esp/overlay_app.cpp
    ${IMGUI_SOURCES})

# Keep one predictable final executable location by default, while allowing
# verification builds to use a separate directory when the release is running.
set(MCP_ESP_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH
    "Directory for the MCP ESP executable")
file(MAKE_DIRECTORY "${MCP_ESP_OUTPUT_DIRECTORY}")
set_target_properties(mcp_esp_overlay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MCP_ESP_OUTPUT_DIRECTORY}")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(mcp_esp_overlay PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${MCP_ESP_OUTPUT_DIRECTORY}")
endforeach()

target_include_directories(mcp_esp_overlay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends")
target_link_libraries(mcp_esp_overlay PRIVATE
    nlohmann_json::nlohmann_json
    d3d11 dxgi d3dcompiler dcomp dwmapi user32 gdi32 shell32)
target_compile_definitions(mcp_esp_overlay PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

if(MSVC)
    target_compile_options(mcp_esp_overlay PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
    target_compile_options(mcp_esp_overlay PRIVATE -Wall -Wextra -Wpedantic)
    # Ship a self-contained MinGW executable. Otherwise WinLibs' POSIX/UCRT
    # build leaves libgcc, libstdc++ and libwinpthread DLL dependencies that
    # are absent on a clean Windows machine.
    target_link_options(mcp_esp_overlay PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS mcp_esp_overlay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES sample_data.json README.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mcp-esp-overlay)
