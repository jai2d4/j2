# FFmpeg discovery that works on Linux and on Windows.
#
# pkg-config is the right tool wherever it exists: it reports the exact versions
# TRACE records in provenance, and it resolves the transitive libraries. It is
# not reliably present on Windows, so this falls back to locating the headers and
# import libraries directly. Either way the result is one target, trace::ffmpeg,
# and one version string per library so the provenance record is identical
# whichever path was taken.

set(TRACE_FFMPEG_COMPONENTS libavformat libavcodec libavutil libswscale libswresample)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET ${TRACE_FFMPEG_COMPONENTS})
endif()

if(TARGET PkgConfig::FFMPEG)
    add_library(trace::ffmpeg ALIAS PkgConfig::FFMPEG)
    set(TRACE_FFMPEG_SOURCE "pkg-config")
    return()
endif()

# ------------------------------------------------------------------- fallback
#
# Version numbers come from the headers rather than being assumed: each library
# publishes its own _VERSION_MAJOR/MINOR/MICRO, and those are what TRACE writes
# into a derived asset's provenance. Guessing them would put a wrong number in an
# evidence record, so a header that cannot be parsed is an error, not a default.
add_library(trace_ffmpeg INTERFACE)

foreach(component IN LISTS TRACE_FFMPEG_COMPONENTS)
    string(TOUPPER ${component} upper)
    # find_library prepends "lib" itself on Unix, so searching for "libavformat"
    # there looks for liblibavformat. Windows import libraries are named without
    # the prefix too, so both spellings are offered and the platform picks.
    string(REGEX REPLACE "^lib" "" bare ${component})

    find_path(${upper}_INCLUDE_DIR
        NAMES ${component}/version.h
        PATH_SUFFIXES ffmpeg
        DOC "Include directory containing ${component}/version.h")
    find_library(${upper}_LIBRARY
        NAMES ${bare} ${component}
        DOC "The ${component} library")

    if(NOT ${upper}_INCLUDE_DIR OR NOT ${upper}_LIBRARY)
        message(FATAL_ERROR
            "FFmpeg component ${component} was not found, and pkg-config is not "
            "available to locate it. Set CMAKE_PREFIX_PATH to an FFmpeg "
            "distribution, or install pkg-config.")
    endif()

    # The three-part version lives in <component>/version.h, except that some
    # releases split the major out into version_major.h; read both when present.
    set(version_headers "${${upper}_INCLUDE_DIR}/${component}/version.h")
    if(EXISTS "${${upper}_INCLUDE_DIR}/${component}/version_major.h")
        list(APPEND version_headers "${${upper}_INCLUDE_DIR}/${component}/version_major.h")
    endif()

    set(version_text "")
    foreach(header IN LISTS version_headers)
        file(READ "${header}" chunk)
        string(APPEND version_text "${chunk}")
    endforeach()

    string(TOUPPER ${component} macro_prefix)
    set(parts "")
    foreach(field MAJOR MINOR MICRO)
        if(version_text MATCHES "#define[ \t]+${macro_prefix}_VERSION_${field}[ \t]+([0-9]+)")
            list(APPEND parts "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    list(LENGTH parts part_count)
    if(NOT part_count EQUAL 3)
        message(FATAL_ERROR
            "Could not read the version of ${component} from its headers. TRACE "
            "records library versions in evidence provenance and will not "
            "substitute a guess.")
    endif()
    string(REPLACE ";" "." ${upper}_VERSION "${parts}")

    # Published under the same names pkg_check_modules would have used, so the
    # provenance and the configuration summary read identically either way.
    set(FFMPEG_${component}_VERSION "${${upper}_VERSION}" CACHE INTERNAL "")

    target_include_directories(trace_ffmpeg INTERFACE ${${upper}_INCLUDE_DIR})
    target_link_libraries(trace_ffmpeg INTERFACE ${${upper}_LIBRARY})
endforeach()

add_library(trace::ffmpeg ALIAS trace_ffmpeg)
set(TRACE_FFMPEG_SOURCE "headers and import libraries")
