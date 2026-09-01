# trace_media — FFmpeg-backed metadata extraction, decoding, playback and export.
#
# Depends on trace_core, never the other way round, and contains no UI code.

add_library(trace_media STATIC
    ffmpeg/ffmpeg_support.cpp
    ffmpeg/media_probe.cpp
    ffmpeg/video_decoder.cpp
    ffmpeg/audio_decoder.cpp
    ffmpeg/encrypted_io.cpp
    ffmpeg/hardware_decode.cpp
    audio/audio_clock.cpp
    audio/waveform.cpp
    audio/waveform_service.cpp
    playback/playback_controller.cpp
    thumbnails/image_writer.cpp
    thumbnails/frame_export_service.cpp
    export/clip_export_service.cpp
    capture/camera.cpp
    capture/capture_session.cpp
    capture/capture_service.cpp
    capture/discovery.cpp
    capture/bluetooth.cpp
)

add_library(trace::media ALIAS trace_media)

target_include_directories(trace_media PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(trace_media PUBLIC trace_core trace::ffmpeg)

# libavdevice is linked when it was found and is an empty interface when it was
# not, so the link line is the same either way and only the define moves. Without
# it TRACE opens network cameras exactly as before and refuses attached ones with
# a stated reason rather than a missing-symbol failure at run time.
target_link_libraries(trace_media PUBLIC trace::avdevice)
if(TRACE_AVDEVICE_FOUND)
    target_compile_definitions(trace_media PUBLIC TRACE_WITH_AVDEVICE=1)
endif()

if(TRACE_WITH_OPENCV)
    target_link_libraries(trace_media PUBLIC ${OpenCV_LIBS})
    target_compile_definitions(trace_media PUBLIC TRACE_WITH_OPENCV=1)
endif()

trace_apply_common_options(trace_media)
