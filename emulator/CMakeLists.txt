cmake_minimum_required(VERSION 3.16)
project(foxtail_emulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(foxtail-emu src/main.cpp)

# Vendored single-header deps + the shared DSP header at the repo root.
target_include_directories(foxtail-emu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

# So the binary can locate the web assets regardless of CWD.
target_compile_definitions(foxtail-emu PRIVATE
    WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")

# httplib needs threads; miniaudio pulls in CoreAudio frameworks on macOS.
find_package(Threads REQUIRED)
target_link_libraries(foxtail-emu PRIVATE Threads::Threads)

if(APPLE)
    target_link_libraries(foxtail-emu PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation")
endif()
