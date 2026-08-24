# trace_media — FFmpeg-backed metadata extraction, decoding, playback and export.
#
# Depends on trace_core, never the other way round, and contains no UI code.

add_library(trace_media STATIC
    ffmpeg/ffmpeg_support.cpp
    ffmpeg/media_probe.cpp
    ffmpeg/video_decoder.cpp
    ffmpeg/audio_decoder.cpp
    audio/waveform.cpp
    audio/waveform_service.cpp
    playback/playback_controller.cpp
    thumbnails/image_writer.cpp
    thumbnails/frame_export_service.cpp
    export/clip_export_service.cpp
)

add_library(trace::media ALIAS trace_media)

target_include_directories(trace_media PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_media PUBLIC trace_core PkgConfig::FFMPEG)

if(TRACE_WITH_OPENCV)
    target_link_libraries(trace_media PUBLIC ${OpenCV_LIBS})
    target_compile_definitions(trace_media PUBLIC TRACE_WITH_OPENCV=1)
endif()

trace_apply_common_options(trace_media)
