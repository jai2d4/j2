# trace_reporting — exhibit bundles and the reports inside them.
#
# Joins core and media the way trace_analysis does: core supplies the records and the
# audit trail, media produces the frames and clips that go in the bundle. Contains no
# Qt, so a bundle can be written and re-verified headlessly — by the test suite today,
# and by a CLI or service later.

add_library(trace_reporting STATIC
    bundle_writer.cpp
    bundle_verifier.cpp
    report_renderer.cpp
    report_service.cpp
)

add_library(trace::reporting ALIAS trace_reporting)

target_include_directories(trace_reporting PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_reporting PUBLIC trace_core trace_media)
trace_apply_common_options(trace_reporting)
