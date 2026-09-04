# trace_ai — the analysis boundary. Interfaces and a registry only: Phase 0
# contains no inference code and no vendor dependencies.

add_library(trace_ai STATIC
    interfaces/provider_registry.cpp
)

add_library(trace::ai ALIAS trace_ai)

target_include_directories(trace_ai PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_ai PUBLIC trace_core)
trace_apply_common_options(trace_ai)
