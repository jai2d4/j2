# The TRACE desktop application.

qt_add_executable(trace_desktop main.cpp)

set_target_properties(trace_desktop PROPERTIES
    OUTPUT_NAME trace
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE OFF)

target_link_libraries(trace_desktop PRIVATE trace_ui trace_core trace_media trace_ai)
trace_apply_common_options(trace_desktop)
