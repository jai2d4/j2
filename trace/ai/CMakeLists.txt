# trace_ai — the analysis boundary.
#
# Phase 0 contributed the interfaces. Phase 1 adds object detection: the
# provider abstraction, model management, pre/post-processing and two
# providers — a deterministic mock and a real local ONNX Runtime detector.
#
# Nothing above this module knows which runtime is in use. Adding TensorRT or a
# remote service later means adding a provider here, not changing evidence,
# storage, timeline or audit code.

add_library(trace_ai STATIC
    interfaces/provider_registry.cpp

    detection/detection_types.cpp
    detection/detection_provider_registry.cpp
    detection/models/class_taxonomy.cpp
    detection/models/model_manager.cpp
    detection/preprocessing/letterbox.cpp
    detection/postprocessing/nms.cpp
    detection/providers/mock_detection_provider.cpp
)

add_library(trace::ai ALIAS trace_ai)

target_include_directories(trace_ai PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_ai PUBLIC trace_core)

# ------------------------------------------------------------- ONNX Runtime
# Optional at build time so the evidence foundation still builds on a machine
# with no inference runtime installed. When absent, the mock provider is the
# only one registered and the UI says so.
if(TRACE_WITH_ONNXRUNTIME)
    if(NOT EXISTS "${TRACE_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
        message(FATAL_ERROR
            "TRACE_WITH_ONNXRUNTIME is ON but no ONNX Runtime was found at "
            "${TRACE_ONNXRUNTIME_ROOT}. Run scripts/fetch_onnxruntime.sh, or configure with "
            "-DTRACE_WITH_ONNXRUNTIME=OFF to build without local inference.")
    endif()

    find_library(TRACE_ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        HINTS "${TRACE_ONNXRUNTIME_ROOT}/lib"
        NO_DEFAULT_PATH
        REQUIRED)

    target_sources(trace_ai PRIVATE detection/providers/onnx_detection_provider.cpp)
    # SYSTEM: the runtime's own headers are not held to TRACE's warning settings.
    target_include_directories(trace_ai SYSTEM PUBLIC "${TRACE_ONNXRUNTIME_ROOT}/include")
    target_link_libraries(trace_ai PUBLIC "${TRACE_ONNXRUNTIME_LIBRARY}")
    target_compile_definitions(trace_ai PUBLIC TRACE_WITH_ONNXRUNTIME=1)

    # Let the built binaries find the shared library without an install step.
    set_target_properties(trace_ai PROPERTIES
        INTERFACE_LINK_DIRECTORIES "${TRACE_ONNXRUNTIME_ROOT}/lib")
endif()

trace_apply_common_options(trace_ai)
