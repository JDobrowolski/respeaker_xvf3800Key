# vsbridge - alternatywa dla build.bat (CMake pobiera RtAudio sam):
#   cmake -B build -A x64      &&  cmake --build build --config Release
#   (MinGW: cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release)
# Na Linuksie buduje sie z ALSA/Pulse, jesli sa naglowki (przydatne do testow protokolu).
cmake_minimum_required(VERSION 3.16)
project(vsbridge CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(rtaudio
  GIT_REPOSITORY https://github.com/thestk/rtaudio.git
  GIT_TAG        6.0.1)
FetchContent_GetProperties(rtaudio)
if(NOT rtaudio_POPULATED)
  FetchContent_Populate(rtaudio)          # tylko zrodla - RtAudio.cpp kompilujemy sami
endif()

add_executable(vsbridge vsbridge.cpp ${rtaudio_SOURCE_DIR}/RtAudio.cpp)
target_include_directories(vsbridge PRIVATE ${rtaudio_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../main)
find_package(Threads REQUIRED)
target_link_libraries(vsbridge PRIVATE Threads::Threads)

if(WIN32)
  target_compile_definitions(vsbridge PRIVATE __WINDOWS_WASAPI__ _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(vsbridge PRIVATE ole32 ksuser mfplat mfuuid wmcodecdspuuid winmm ws2_32)
  if(MSVC)
    set_property(TARGET vsbridge PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
else()
  find_package(ALSA QUIET)
  if(ALSA_FOUND)
    target_compile_definitions(vsbridge PRIVATE __LINUX_ALSA__)
    target_link_libraries(vsbridge PRIVATE ALSA::ALSA)
  endif()
endif()
