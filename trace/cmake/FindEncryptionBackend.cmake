# Encryption backend for TRACE: SQLCipher for the case database, libcrypto for
# evidence containers and key wrapping.
#
# Produces, when everything is present:
#
#   trace::encryption   an interface target carrying both, and the
#                       TRACE_WITH_ENCRYPTION compile definition
#   TRACE_ENCRYPTION_FOUND    TRUE
#   TRACE_SQLCIPHER_VERSION   for the configuration summary
#
# When something is missing this module does not fail the build. TRACE without
# encryption is a working product — it simply cannot create or open an encrypted
# workspace, and says so in as many words rather than failing obscurely later.
# What it must never do is build *believing* it can encrypt when it cannot, so
# the compile definition and the runtime `crypto::available()` come from the
# same place.
#
# SQLCipher replaces SQLite rather than sitting alongside it: it exports the same
# symbol names, so linking both is an ODR violation that may or may not show up
# as a crash. Callers link trace::sqlite, which is one or the other.

include(FindPackageHandleStandardArgs)

set(TRACE_ENCRYPTION_FOUND FALSE)
set(TRACE_SQLCIPHER_VERSION "")

if(NOT TRACE_WITH_ENCRYPTION)
    return()
endif()

# ------------------------------------------------------------------ libcrypto
#
# OpenSSL's crypto half only. TRACE speaks no TLS and has no use for libssl.
find_package(OpenSSL COMPONENTS Crypto)
if(NOT OPENSSL_FOUND)
    message(STATUS "TRACE: OpenSSL not found — encryption will be unavailable")
    return()
endif()

# ------------------------------------------------------------------ SQLCipher
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_SQLCIPHER QUIET sqlcipher)
endif()

if(PC_SQLCIPHER_FOUND)
    set(TRACE_SQLCIPHER_VERSION ${PC_SQLCIPHER_VERSION})
    set(_sqlcipher_include_hint ${PC_SQLCIPHER_INCLUDE_DIRS})
    set(_sqlcipher_library_hint ${PC_SQLCIPHER_LIBRARY_DIRS})
else()
    set(_sqlcipher_include_hint "")
    set(_sqlcipher_library_hint "")
endif()

# SQLCipher ships its header as sqlite3.h inside a sqlcipher/ directory, because
# it is a fork of SQLite and keeps the API. Search for that path specifically:
# finding a bare sqlite3.h would silently pick up system SQLite, which compiles
# and links and then cannot open an encrypted database.
find_path(TRACE_SQLCIPHER_INCLUDE_DIR
    NAMES sqlite3.h
    HINTS ${_sqlcipher_include_hint}
    PATH_SUFFIXES sqlcipher
    DOC "Directory containing SQLCipher's sqlite3.h")

find_library(TRACE_SQLCIPHER_LIBRARY
    NAMES sqlcipher libsqlcipher
    HINTS ${_sqlcipher_library_hint}
    DOC "The SQLCipher library")

if(NOT TRACE_SQLCIPHER_INCLUDE_DIR OR NOT TRACE_SQLCIPHER_LIBRARY)
    message(STATUS "TRACE: SQLCipher not found — encryption will be unavailable")
    return()
endif()

# Refuse a header that is not actually SQLCipher's. find_path with a suffix can
# still land on plain SQLite if a distribution installs it under sqlcipher/, and
# the failure that would cause happens at "PRAGMA key", long after the build.
if(NOT EXISTS "${TRACE_SQLCIPHER_INCLUDE_DIR}/sqlite3.h")
    message(STATUS "TRACE: SQLCipher header missing — encryption will be unavailable")
    return()
endif()
file(READ "${TRACE_SQLCIPHER_INCLUDE_DIR}/sqlite3.h" _sqlcipher_header_text)
string(FIND "${_sqlcipher_header_text}" "sqlite3_key" _sqlcipher_marker)
unset(_sqlcipher_header_text)
if(_sqlcipher_marker EQUAL -1)
    message(STATUS
        "TRACE: header at ${TRACE_SQLCIPHER_INCLUDE_DIR} is SQLite, not SQLCipher — "
        "encryption will be unavailable")
    return()
endif()

if(NOT TRACE_SQLCIPHER_VERSION)
    # No pkg-config: read the version out of the header rather than assume one.
    file(STRINGS "${TRACE_SQLCIPHER_INCLUDE_DIR}/sqlite3.h" _version_line
         REGEX "^#define[ \t]+SQLITE_VERSION[ \t]+\"[^\"]+\"")
    if(_version_line MATCHES "\"([^\"]+)\"")
        set(TRACE_SQLCIPHER_VERSION "${CMAKE_MATCH_1}")
    else()
        set(TRACE_SQLCIPHER_VERSION "unknown")
    endif()
endif()

add_library(trace_sqlcipher INTERFACE)
target_include_directories(trace_sqlcipher INTERFACE ${TRACE_SQLCIPHER_INCLUDE_DIR})
target_link_libraries(trace_sqlcipher INTERFACE ${TRACE_SQLCIPHER_LIBRARY})
add_library(trace::sqlcipher ALIAS trace_sqlcipher)

add_library(trace_encryption INTERFACE)
target_link_libraries(trace_encryption INTERFACE OpenSSL::Crypto)
target_compile_definitions(trace_encryption INTERFACE TRACE_WITH_ENCRYPTION=1)
add_library(trace::encryption ALIAS trace_encryption)

set(TRACE_ENCRYPTION_FOUND TRUE)
