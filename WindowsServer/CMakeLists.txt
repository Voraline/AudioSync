cmake_minimum_required(VERSION 3.15)
project(AudioSyncServer C)

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

add_executable(AudioSyncServer main.c)

target_compile_definitions(AudioSyncServer PRIVATE NDEBUG)

target_compile_options(AudioSyncServer PRIVATE
    $<$<CONFIG:Release>:/O2>
    $<$<CONFIG:Release>:/GL>
    $<$<CONFIG:Release>:/Gy>
    $<$<CONFIG:Release>:/fp:precise>
    $<$<CONFIG:Release>:/fp:contract>
    $<$<CONFIG:Release>:/arch:AVX2>
    /W4
)

target_link_options(AudioSyncServer PRIVATE
    $<$<CONFIG:Release>:/LTCG>
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
)

target_link_libraries(AudioSyncServer PRIVATE ws2_32 winmm)
