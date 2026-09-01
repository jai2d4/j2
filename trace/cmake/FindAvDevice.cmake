# libavdevice, which is what lets TRACE open a camera attached to this machine.
#
# Optional and degrading, for the same reason the encryption backend is: the
# library is genuinely absent on plenty of systems (Debian and Ubuntu ship it in
# a separate libavdevice-dev package, and some FFmpeg builds are configured
# without it), and a forensic tool that refuses to build because one of three
# camera transports is unavailable is worse than one that builds and says which
# transport it cannot offer.
#
# Network cameras — the RTSP/RTMP/SRT/HTTP path, which is what an IP camera on
# Ethernet or WiFi uses — need only libavformat and are unaffected by this. What
# is lost without avdevice is USB webcams and capture cards: video4linux2 on
# Linux, DirectShow on Windows, AVFoundation on macOS. The code checks
# TRACE_WITH_AVDEVICE and names that limitation where an operator meets it,
# rather than failing later inside FFmpeg as "Protocol not found".
#
# Produces:
#   trace::avdevice        when found (an INTERFACE target, safe to link always
#                          via the guard below)
#   TRACE_AVDEVICE_FOUND   TRUE/FALSE
#   TRACE_AVDEVICE_VERSION the version string, read from the library itself

set(TRACE_AVDEVICE_FOUND FALSE)
set(TRACE_AVDEVICE_VERSION "")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(TRACE_AVDEVICE_PC IMPORTED_TARGET QUIET libavdevice)
endif()

if(TARGET PkgConfig::TRACE_AVDEVICE_PC)
    add_library(trace_avdevice INTERFACE)
    target_link_libraries(trace_avdevice INTERFACE PkgConfig::TRACE_AVDEVICE_PC)
    add_library(trace::avdevice ALIAS trace_avdevice)
    set(TRACE_AVDEVICE_FOUND TRUE)
    set(TRACE_AVDEVICE_VERSION "${TRACE_AVDEVICE_PC_VERSION}")
else()
    # No pkg-config, or no .pc file: look for the header and library directly,
    # the same fallback FindFFmpegPortable uses on Windows.
    find_path(TRACE_AVDEVICE_INCLUDE_DIR
        NAMES libavdevice/avdevice.h
        PATH_SUFFIXES ffmpeg
        DOC "Include directory containing libavdevice/avdevice.h")
    find_library(TRACE_AVDEVICE_LIBRARY
        NAMES avdevice libavdevice
        DOC "The libavdevice library")

    if(TRACE_AVDEVICE_INCLUDE_DIR AND TRACE_AVDEVICE_LIBRARY)
        add_library(trace_avdevice INTERFACE)
        target_include_directories(trace_avdevice INTERFACE ${TRACE_AVDEVICE_INCLUDE_DIR})
        target_link_libraries(trace_avdevice INTERFACE ${TRACE_AVDEVICE_LIBRARY})
        add_library(trace::avdevice ALIAS trace_avdevice)
        set(TRACE_AVDEVICE_FOUND TRUE)

        # The version comes out of the header rather than being assumed, because
        # it is written into the provenance of a captured recording and a guessed
        # number there is a wrong number in an evidence record.
        set(version_headers "${TRACE_AVDEVICE_INCLUDE_DIR}/libavdevice/version.h")
        if(EXISTS "${TRACE_AVDEVICE_INCLUDE_DIR}/libavdevice/version_major.h")
            list(APPEND version_headers "${TRACE_AVDEVICE_INCLUDE_DIR}/libavdevice/version_major.h")
        endif()
        set(version_text "")
        foreach(header IN LISTS version_headers)
            if(EXISTS "${header}")
                file(READ "${header}" chunk)
                string(APPEND version_text "${chunk}")
            endif()
        endforeach()
        set(parts "")
        foreach(field MAJOR MINOR MICRO)
            if(version_text MATCHES "#define[ \t]+LIBAVDEVICE_VERSION_${field}[ \t]+([0-9]+)")
                list(APPEND parts "${CMAKE_MATCH_1}")
            endif()
        endforeach()
        list(LENGTH parts part_count)
        if(part_count EQUAL 3)
            list(JOIN parts "." TRACE_AVDEVICE_VERSION)
        else()
            # Found but unreadable: usable for opening a camera, but nothing may
            # claim a version it could not read.
            set(TRACE_AVDEVICE_VERSION "unknown")
        endif()
    endif()
endif()

if(NOT TRACE_AVDEVICE_FOUND)
    # An INTERFACE target with nothing in it, so media/CMakeLists.txt links the
    # same name either way and the difference lives in one place.
    add_library(trace_avdevice INTERFACE)
    add_library(trace::avdevice ALIAS trace_avdevice)
endif()
