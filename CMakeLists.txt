cmake_minimum_required(VERSION 3.20)
project(AudioBridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8)
    # Enable AVX2 / Enhanced instruction set if desired, keep standard compatible
endif()

# Configure RtAudio options before adding it
set(RTAUDIO_API_WASAPI ON CACHE BOOL "Build WASAPI API" FORCE)
set(RTAUDIO_API_ASIO ON CACHE BOOL "Build ASIO API" FORCE)
set(RTAUDIO_API_DS OFF CACHE BOOL "Build DirectSound API" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static library" FORCE)

# Look for locally unpacked RtAudio in extern/rtaudio, or use FetchContent
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/rtaudio/CMakeLists.txt")
    message(STATUS "[AudioBridge] Using local RtAudio sources from extern/rtaudio")
    add_subdirectory(extern/rtaudio EXCLUDE_FROM_ALL)
else()
    message(STATUS "[AudioBridge] Fetching RtAudio 6.0.1 via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        rtaudio
        URL https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.zip
    )
    FetchContent_MakeAvailable(rtaudio)
endif()

add_executable(audiobridge
    main.cpp
)

target_include_directories(audiobridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/rtaudio
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/rtaudio/include
)

target_link_libraries(audiobridge PRIVATE
    rtaudio
    ole32
    avrt
    mmdevapi
    ksuser
    winmm
)
