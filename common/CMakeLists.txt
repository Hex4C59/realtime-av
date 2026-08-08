add_library(avcommon STATIC
    rtp/rtp_packetizer.cpp
    rtp/rtp_depacketizer.cpp
    net/udp_socket.cpp
)
target_include_directories(avcommon PUBLIC ${CMAKE_SOURCE_DIR})
