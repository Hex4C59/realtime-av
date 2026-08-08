find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Network)

set(CMAKE_AUTOMOC ON)

pkg_check_modules(AUDIO REQUIRED IMPORTED_TARGET libpulse-simple opus)

add_executable(avclient
    main.cpp
    audio/audio_capture.cpp
    audio/audio_player.cpp
    audio/opus_codec.cpp
    capture/v4l2_capture.cpp
    codec/video_encoder.cpp
    codec/video_decoder.cpp
    media/jitter_buffer.cpp
    render/yuv_widget.cpp
    signal/signal_client.cpp
    ui/main_window.cpp
)

target_include_directories(avclient PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(avclient PRIVATE
    avcommon
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Network
    PkgConfig::FFMPEG
    PkgConfig::AUDIO
)
