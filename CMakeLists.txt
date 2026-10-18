cmake_minimum_required(VERSION 3.20)
project(factorio_hudless_probe CXX)
if(WIN32)
  enable_language(RC) # version resource
endif()

# ReShade add-on headers and the matching Dear ImGui header are vendored in third_party/.
# Point RESHADE_DIR at a ReShade checkout instead if you want to build against a different ReShade version.
set(RESHADE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade" CACHE PATH "Directory containing ReShade's include/")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "Directory containing imgui.h")
# NVIDIA Streamline SDK headers (MIT), matching the Streamline 2.14.16 release DLLs.
set(STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/streamline" CACHE PATH "Directory containing Streamline's include/")

if(NOT EXISTS "${RESHADE_DIR}/include/reshade.hpp")
  message(FATAL_ERROR "reshade.hpp not found in ${RESHADE_DIR}/include")
endif()
if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
  message(FATAL_ERROR "imgui.h not found in ${IMGUI_DIR}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(hudless_probe SHARED
  src/hudless_probe.cpp
  src/bridge.cpp
  src/bridge.h
  src/fg_streamline.cpp
  src/fg_streamline.h
  src/motion.cpp
  src/motion.h
  src/version.h
  src/version.rc
  src/setup_check.cpp
  src/setup_check.h)
target_include_directories(hudless_probe PRIVATE "${RESHADE_DIR}/include" "${IMGUI_DIR}" "${STREAMLINE_DIR}/include")
target_compile_definitions(hudless_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(hudless_probe PRIVATE d3d11 d3d12 dxgi wintrust crypt32 advapi32)

# Release build: proper name, developer tools hidden by default. Developer build (default): hudless_probe.addon64.
option(FG_RELEASE "Build factorio_fg.addon64 (release) instead of hudless_probe.addon64 (developer)" OFF)
if(FG_RELEASE)
  target_compile_definitions(hudless_probe PRIVATE FG_RELEASE)
  set(FG_OUTPUT_NAME "factorio_fg")
else()
  set(FG_OUTPUT_NAME "hudless_probe")
endif()

# ReShade loads add-ons with the .addon64 extension
set_target_properties(hudless_probe PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${FG_OUTPUT_NAME}"
  SUFFIX ".addon64"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  target_compile_options(hudless_probe PRIVATE /W4 /permissive- /utf-8)
endif()
