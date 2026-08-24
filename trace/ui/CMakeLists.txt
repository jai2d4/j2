# trace_ui — Qt widgets. All domain work is delegated to trace_core/trace_media
# through ApplicationContext; no SQL, hashing or decoding lives in this layer.

set(CMAKE_AUTOMOC ON)
# TRACE builds its interface in code; there are no .ui files to compile.
set(CMAKE_AUTOUIC OFF)

add_library(trace_ui STATIC
    common/theme.cpp
    common/display_utils.cpp
    common/background_task.cpp
    common/ingest_dialog.cpp
    common/settings_dialog.cpp
    common/detection_overlay.cpp

    app/application_context.cpp
    audio/audio_output.cpp
    app/main_window.cpp

    case_browser/case_dialog.cpp
    case_browser/case_browser_panel.cpp
    evidence_browser/evidence_panel.cpp
    viewer/video_view.cpp
    viewer/playback_bridge.cpp
    viewer/viewer_panel.cpp
    timeline/timeline_widget.cpp
    inspector/inspector_panel.cpp
    analysis/analysis_panel.cpp
    analysis/detections_panel.cpp
    analysis/detection_inspector.cpp
    reports/report_builder_dialog.cpp
    reports/qt_document_renderer.cpp
    annotations/bookmark_dialog.cpp
    annotations/annotation_dialog.cpp
    annotations/bookmarks_panel.cpp
    annotations/annotations_panel.cpp
    audit_viewer/audit_panel.cpp
)

add_library(trace::ui ALIAS trace_ui)

target_include_directories(trace_ui PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_ui PUBLIC
    trace_core
    trace_media
    trace_ai
    trace_analysis
    trace_reporting
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
)
trace_apply_common_options(trace_ui)
