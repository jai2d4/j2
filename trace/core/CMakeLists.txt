# trace_core — evidence domain, persistence, provenance and audit.
#
# Deliberately free of Qt and FFmpeg: the domain must be testable headlessly and
# must not depend on either the UI toolkit or a particular media library.

set(TRACE_MIGRATIONS_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/migrations_embedded.cpp)
file(GLOB TRACE_MIGRATION_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/migrations/*.sql)

add_custom_command(
    OUTPUT ${TRACE_MIGRATIONS_GENERATED}
    COMMAND ${CMAKE_COMMAND}
            -DMIGRATIONS_DIR=${PROJECT_SOURCE_DIR}/migrations
            -DOUTPUT_FILE=${TRACE_MIGRATIONS_GENERATED}
            -P ${PROJECT_SOURCE_DIR}/scripts/embed_migrations.cmake
    DEPENDS ${TRACE_MIGRATION_FILES} ${PROJECT_SOURCE_DIR}/scripts/embed_migrations.cmake
    COMMENT "Embedding TRACE schema migrations"
    VERBATIM)

add_custom_target(trace_migrations_generated DEPENDS ${TRACE_MIGRATIONS_GENERATED})

add_library(trace_core STATIC
    common/result.cpp
    common/uuid.cpp
    common/time_utils.cpp
    common/json.cpp
    common/logging.cpp
    common/string_utils.cpp

    security/sha256.cpp
    security/password.cpp
    security/file_hasher.cpp
    security/user_context.cpp

    database/database.cpp
    database/migration_runner.cpp
    ${TRACE_MIGRATIONS_GENERATED}

    models/enums.cpp

    repositories/case_repository.cpp
    repositories/user_repository.cpp
    repositories/evidence_repository.cpp
    repositories/metadata_repository.cpp
    repositories/bookmark_repository.cpp
    repositories/annotation_repository.cpp
    repositories/audit_repository.cpp
    repositories/provenance_repository.cpp
    repositories/analysis_repository.cpp
    repositories/report_repository.cpp
    repositories/settings_repository.cpp
    repositories/user_repository.cpp

    storage/storage_layout.cpp
    storage/file_operations.cpp

    services/audit_service.cpp
    services/case_service.cpp
    services/evidence_service.cpp
    services/integrity_service.cpp
    services/annotation_service.cpp
    services/derived_asset_service.cpp
    services/analysis_service.cpp

    settings/settings_service.cpp
    settings/system_capabilities.cpp
)

add_library(trace::core ALIAS trace_core)

target_include_directories(trace_core PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_core PUBLIC SQLite::SQLite3 trace_version Threads::Threads)
trace_apply_common_options(trace_core)
