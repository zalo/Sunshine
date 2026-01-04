# libdatachannel dependency for WebRTC support

# Options for libdatachannel
set(NO_WEBSOCKET ON CACHE BOOL "Disable WebSocket support in libdatachannel")
set(NO_EXAMPLES ON CACHE BOOL "Disable examples")
set(NO_TESTS ON CACHE BOOL "Disable tests")
set(USE_NICE OFF CACHE BOOL "Use libnice instead of libjuice")

# Add libdatachannel subdirectory
if(EXISTS "${CMAKE_SOURCE_DIR}/third-party/libdatachannel/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/libdatachannel" EXCLUDE_FROM_ALL)

    # Include directories for libdatachannel
    include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/libdatachannel/include")

    message(STATUS "WebRTC: libdatachannel enabled")
else()
    message(WARNING "WebRTC: libdatachannel not found, WebRTC support disabled")
endif()
