# TRACE test suites.
#
#   trace_unit_tests        — domain logic, no filesystem beyond scratch dirs
#   trace_integration_tests — real ingestion, decoding and integrity checks
#   trace_acceptance_test   — the Phase 0 workflow driven through the real UI

find_package(GTest REQUIRED)
include(GoogleTest)

add_library(trace_test_support STATIC support/test_environment.cpp)
target_include_directories(trace_test_support PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_test_support PUBLIC trace_core trace_media trace_ai trace_analysis trace_reporting)
target_compile_definitions(trace_test_support PUBLIC
    TRACE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    TRACE_TEST_MODELS_DIR="${PROJECT_SOURCE_DIR}/models")

# ------------------------------------------------------------------ unit tests
add_executable(trace_unit_tests
    unit/sha256_test.cpp
    unit/password_test.cpp
    unit/common_test.cpp
    unit/storage_layout_test.cpp
    unit/database_test.cpp
    unit/repository_test.cpp
    unit/service_test.cpp
    unit/detection_test.cpp
)
target_link_libraries(trace_unit_tests PRIVATE
    trace_test_support trace_core trace_media trace_ai trace_analysis
    GTest::gtest GTest::gtest_main)
trace_apply_common_options(trace_unit_tests)
gtest_discover_tests(trace_unit_tests
    DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS unit)

# ----------------------------------------------------------- integration tests
add_executable(trace_integration_tests
    integration/ingestion_test.cpp
    integration/persistence_test.cpp
    integration/integrity_failure_test.cpp
    integration/media_test.cpp
    integration/audio_test.cpp
    integration/detection_pipeline_test.cpp
    integration/report_export_test.cpp
)
target_link_libraries(trace_integration_tests PRIVATE
    trace_test_support trace_core trace_media trace_ai trace_reporting
    GTest::gtest GTest::gtest_main)
trace_apply_common_options(trace_integration_tests)
gtest_discover_tests(trace_integration_tests
    DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS integration)

# ------------------------------------------------------------- acceptance test
if(TRACE_BUILD_DESKTOP)
    set(CMAKE_AUTOMOC ON)
    add_executable(trace_acceptance_test integration/acceptance_phase0_test.cpp)
    target_link_libraries(trace_acceptance_test PRIVATE
        trace_test_support trace_ui trace_core trace_media trace_ai trace_analysis
        Qt6::Core Qt6::Gui Qt6::Widgets GTest::gtest)
    trace_apply_common_options(trace_acceptance_test)

    # The workflow needs a windowing system; offscreen keeps it runnable on a
    # build machine, and a real X server (xvfb) is used when screenshots matter.
    add_test(NAME trace_acceptance_phase0 COMMAND trace_acceptance_test)
    set_tests_properties(trace_acceptance_phase0 PROPERTIES
        LABELS acceptance
        TIMEOUT 600
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Widget-level unit tests: overlay geometry and the no-modification rule.
    add_executable(trace_ui_unit_tests unit/overlay_test.cpp)
    target_link_libraries(trace_ui_unit_tests PRIVATE
        trace_ui trace_core Qt6::Core Qt6::Gui Qt6::Widgets GTest::gtest)
    trace_apply_common_options(trace_ui_unit_tests)
    # The binary selects the offscreen platform for itself when none is set, so
    # both discovery and execution work on a machine with no display.
    gtest_discover_tests(trace_ui_unit_tests
        DISCOVERY_TIMEOUT 60
        PROPERTIES LABELS unit)

    # Phase 1: object detection driven through the same real window.
    add_executable(trace_acceptance_phase1_test integration/acceptance_phase1_test.cpp)
    target_link_libraries(trace_acceptance_phase1_test PRIVATE
        trace_test_support trace_ui trace_core trace_media trace_ai trace_analysis
        Qt6::Core Qt6::Gui Qt6::Widgets GTest::gtest)
    trace_apply_common_options(trace_acceptance_phase1_test)

    add_test(NAME trace_acceptance_phase1 COMMAND trace_acceptance_phase1_test)
    set_tests_properties(trace_acceptance_phase1 PROPERTIES
        LABELS acceptance
        TIMEOUT 1800
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Phase 2: exhibit export driven through the real dialog.
    add_executable(trace_acceptance_phase2_test integration/acceptance_phase2_test.cpp)
    target_link_libraries(trace_acceptance_phase2_test PRIVATE
        trace_test_support trace_ui trace_core trace_media trace_ai trace_analysis
        trace_reporting Qt6::Core Qt6::Gui Qt6::Widgets GTest::gtest)
    trace_apply_common_options(trace_acceptance_phase2_test)

    add_test(NAME trace_acceptance_phase2 COMMAND trace_acceptance_phase2_test)
    set_tests_properties(trace_acceptance_phase2 PROPERTIES
        LABELS acceptance
        TIMEOUT 900
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Phase 3: audio — the waveform as a derived asset, and what the transport
    # can honestly claim on the machine the test is running on.
    add_executable(trace_acceptance_phase3_test integration/acceptance_phase3_test.cpp)
    target_link_libraries(trace_acceptance_phase3_test PRIVATE
        trace_test_support trace_ui trace_core trace_media trace_ai trace_analysis
        trace_reporting Qt6::Core Qt6::Gui Qt6::Widgets GTest::gtest)
    trace_apply_common_options(trace_acceptance_phase3_test)

    add_test(NAME trace_acceptance_phase3 COMMAND trace_acceptance_phase3_test)
    set_tests_properties(trace_acceptance_phase3 PROPERTIES
        LABELS acceptance
        TIMEOUT 900
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
