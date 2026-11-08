# n64net: shared netplay client pieces for native N64 ports.
#
#   add_subdirectory(n64net)            # after the game's SDL2 target exists
#   target_link_libraries(<game> PRIVATE n64net)
#
# Options:
#   N64NET_VOICE (ON)        voice chat (needs SDL2 for the microphone; builds Opus)
#   N64NET_SDL2_TARGET       the SDL2 target to link (default: the first of
#                            SDL2::SDL2-static, SDL2::SDL2, SDL2-static, SDL2 that exists)
cmake_minimum_required(VERSION 3.17)
project(n64net LANGUAGES C CXX)

option(N64NET_VOICE "Build n64net voice chat (SDL2 microphone + Opus)" ON)

add_library(n64net STATIC
    client/src/n64net_rendezvous.cpp
    client/src/n64net_hmac.h
    client/include/n64net_rendezvous.h)
target_include_directories(n64net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/client/include)
target_compile_features(n64net PRIVATE cxx_std_17)

if (N64NET_VOICE)
    target_sources(n64net PRIVATE client/src/n64net_voice.cpp client/include/n64net_voice.h)

    if (NOT N64NET_SDL2_TARGET)
        foreach (candidate SDL2::SDL2-static SDL2::SDL2 SDL2-static SDL2)
            if (TARGET ${candidate})
                set(N64NET_SDL2_TARGET ${candidate})
                break()
            endif()
        endforeach()
    endif()
    if (NOT N64NET_SDL2_TARGET)
        find_package(SDL2 REQUIRED)
        set(N64NET_SDL2_TARGET SDL2::SDL2)
    endif()
    target_link_libraries(n64net PRIVATE ${N64NET_SDL2_TARGET})

    # Opus, built from source so every platform uses the same codec (BSD).
    if (NOT TARGET opus)
        include(FetchContent)
        set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(opus
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG        v1.5.2
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(opus)
    endif()
    target_link_libraries(n64net PRIVATE opus)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/N64NetBuildId.cmake)
