# libdatachannel dependency for WebRTC support

# Options for libdatachannel
set(NO_WEBSOCKET ON CACHE BOOL "Disable WebSocket support in libdatachannel")
set(NO_EXAMPLES ON CACHE BOOL "Disable examples")
set(NO_TESTS ON CACHE BOOL "Disable tests")
set(USE_NICE OFF CACHE BOOL "Use libnice instead of libjuice")

# Add libdatachannel subdirectory
if(EXISTS "${CMAKE_SOURCE_DIR}/third-party/libdatachannel/CMakeLists.txt")
    # Disable -Werror for third-party code (libsrtp has format warnings on Windows)
    if(WIN32)
        set(CMAKE_C_FLAGS_BACKUP "${CMAKE_C_FLAGS}")
        set(CMAKE_CXX_FLAGS_BACKUP "${CMAKE_CXX_FLAGS}")
        # Remove -Werror from flags for third-party build
        string(REPLACE "-Werror" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
        string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        # Also suppress the specific format warning in libsrtp
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-format")
    endif()

    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/libdatachannel" EXCLUDE_FROM_ALL)

    # Restore flags after third-party build
    if(WIN32)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_BACKUP}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_BACKUP}")
    endif()

    # Include directories for libdatachannel
    include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/libdatachannel/include")

    message(STATUS "WebRTC: libdatachannel enabled")
else()
    message(WARNING "WebRTC: libdatachannel not found, WebRTC support disabled")
endif()
