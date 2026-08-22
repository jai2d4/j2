# trace_analysis — the detection pipeline.
#
# Joins the three lower modules: media decodes, ai detects, core persists and
# audits. Contains no Qt, so the whole analysis path is testable headlessly.

add_library(trace_analysis STATIC
    frame_sampler.cpp
    analysis_pipeline.cpp
)

add_library(trace::analysis ALIAS trace_analysis)

target_include_directories(trace_analysis PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_analysis PUBLIC trace_core trace_media trace_ai)
trace_apply_common_options(trace_analysis)
