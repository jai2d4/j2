# TRACE — AI video evidence intelligence platform
# Phase 0 foundation build.
cmake_minimum_required(VERSION 3.21)

project(TRACE
    VERSION 0.2.0
    DESCRIPTION "TRACE — video evidence intelligence platform"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(TRACE_BUILD_DESKTOP "Build the Qt desktop application" ON)
option(TRACE_BUILD_TESTS   "Build the automated test suites"   ON)
option(TRACE_WITH_OPENCV   "Enable optional OpenCV-backed helpers" OFF)

# Local AI inference (Phase 1). Optional so the evidence foundation still builds
# on a machine with no runtime installed; when off, only the deterministic mock
# detection provider is registered.
option(TRACE_WITH_ONNXRUNTIME "Build the local ONNX Runtime detection provider" ON)
set(TRACE_ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime"
    CACHE PATH "Root of an ONNX Runtime distribution (include/ and lib/)")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/TraceWarnings.cmake)

# ---------------------------------------------------------------- dependencies
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# FFmpeg via pkg-config where it exists, headers and import libraries where it
# does not. Both paths produce trace::ffmpeg and the same version strings.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindFFmpegPortable.cmake)

if(TRACE_WITH_OPENCV)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
endif()

if(TRACE_BUILD_DESKTOP)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
    qt_standard_project_setup()
    set(CMAKE_AUTOUIC OFF)
endif()

# ------------------------------------------------------------------- version.h
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/trace_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/trace/trace_version.h
    @ONLY)

add_library(trace_version INTERFACE)
target_include_directories(trace_version INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# ------------------------------------------------------------------ subprojects
add_subdirectory(core)
add_subdirectory(media)
add_subdirectory(ai)
add_subdirectory(analysis)
add_subdirectory(reporting)

if(TRACE_BUILD_DESKTOP)
    add_subdirectory(ui)
    add_subdirectory(apps/desktop)
endif()

if(TRACE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "TRACE ${PROJECT_VERSION} configuration")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  desktop app ........ ${TRACE_BUILD_DESKTOP}")
message(STATUS "  tests .............. ${TRACE_BUILD_TESTS}")
message(STATUS "  SQLite ............. ${SQLite3_VERSION}")
message(STATUS "  FFmpeg (libav) ..... avformat ${FFMPEG_libavformat_VERSION}, avcodec ${FFMPEG_libavcodec_VERSION} (via ${TRACE_FFMPEG_SOURCE})")
if(TRACE_BUILD_DESKTOP)
    message(STATUS "  Qt ................. ${Qt6_VERSION}")
endif()
message(STATUS "  OpenCV helpers ..... ${TRACE_WITH_OPENCV}")
message(STATUS "  ONNX Runtime ....... ${TRACE_WITH_ONNXRUNTIME}")
if(TRACE_WITH_ONNXRUNTIME)
    message(STATUS "    root ............. ${TRACE_ONNXRUNTIME_ROOT}")
endif()
message(STATUS "")
