cmake_minimum_required(VERSION 3.15)
project(AudioSyncServer C)
add_executable(AudioSyncServer main.c)
if(MSVC)
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/Od"   "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/GS"   "" CMAKE_C_FLAGS       "${CMAKE_C_FLAGS}")
    target_compile_options(AudioSyncServer PRIVATE
        /O2
        /Ob3
        /Ot
        /Oi
        /Oy
        /Gy
        /Gw
        /GL
        /arch:AVX2
        /fp:fast
        /GS-
        /sdl-
        /Zc:inline
        /Qpar
        /favor:AMD64
        /DNDEBUG
    )
    target_link_options(AudioSyncServer PRIVATE
        /LTCG
        /OPT:REF
        /OPT:ICF
        /INCREMENTAL:NO
    )
endif()