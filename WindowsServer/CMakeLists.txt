cmake_minimum_required(VERSION 3.15)
project(AudioSyncServer C)
add_executable(AudioSyncServer main.c)

if(MSVC)
    target_compile_options(AudioSyncServer PRIVATE
        /O2
        /fp:precise
        /W3
    )
else()
    target_compile_options(AudioSyncServer PRIVATE
        -O2
        -ffp-contract=off
        -Wall
    )
endif()