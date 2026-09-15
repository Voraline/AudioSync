cmake_minimum_required(VERSION 3.15)
project(AudioSyncServer C)

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

add_executable(AudioSyncServer main.c)

target_compile_definitions(AudioSyncServer PRIVATE NDEBUG)

target_link_libraries(AudioSyncServer PRIVATE ws2_32 winmm)
