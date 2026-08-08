cmake_minimum_required(VERSION 3.16)
project(realtime-av CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

# Debug 构建默认开 AddressSanitizer,内存错误当场报错而不是随机崩溃
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    add_compile_options(-fsanitize=address -g)
    add_link_options(-fsanitize=address)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG IMPORTED_TARGET
    libavcodec
    libavutil
    libswscale
)

if(NOT FFMPEG_FOUND)
    message(FATAL_ERROR
        "未找到 FFmpeg 开发包。请先安装依赖:\n"
        "  sudo apt install libavcodec-dev libavformat-dev libavutil-dev \\\n"
        "      libswscale-dev libavdevice-dev libopus-dev libx264-dev \\\n"
        "      qt6-base-dev libpulse-dev libv4l-dev v4l-utils ffmpeg\n"
        "然后重新运行 cmake。")
endif()

add_subdirectory(common)
add_subdirectory(examples)
add_subdirectory(client)
add_subdirectory(signal_server)
add_subdirectory(sfu_server)
add_subdirectory(tests)
