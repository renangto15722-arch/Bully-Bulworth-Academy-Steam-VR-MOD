# Bully VR PC - a VR mod for Bully: Scholarship Edition (Steam, 1.154, 32-bit,
# Gamebryo / Direct3D 9). Built as d3d9.dll, dropped next to Bully.exe: the
# game loads it by name instead of the system one.
#
# Configure (32-bit only, the game is x86):
#   cmake -S . -B K:/bullyvrpc_build -A Win32
#   cmake --build K:/bullyvrpc_build --config RelWithDebInfo
cmake_minimum_required(VERSION 3.20)
project(BullyVRPC C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Bully.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: nothing to install next to the game but the DLL itself
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# OpenXR loader (Apache-2.0), linked in statically so no loader DLL ships
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/OpenXR-SDK EXCLUDE_FROM_ALL)
set_target_properties(openxr_loader PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(d3d9 SHARED
    src/proxy_d3d9.cpp
    src/actions.cpp
    src/body.cpp
    src/input.cpp
    src/gamecam.cpp
    src/hud.cpp
    src/config.cpp
    src/d3d_trace.cpp
    src/game_camera.cpp
    src/log.cpp
    src/menu.cpp
    src/fight.cpp
    src/stereo.cpp
    src/vk_bridge.cpp
    src/vr_main.cpp
    src/vr_openxr.cpp
    src/d3d9.def
)
set_target_properties(d3d9 PROPERTIES OUTPUT_NAME d3d9)
target_include_directories(d3d9 PRIVATE src third_party/vulkan/include third_party/OpenXR-SDK/include)
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(d3d9 PRIVATE openxr_loader d3d11 dxgi gdi32)
