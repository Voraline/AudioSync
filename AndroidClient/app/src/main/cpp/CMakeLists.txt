cmake_minimum_required(VERSION 3.22.1)
project("audiosync" C)
add_library(audiosync SHARED native-lib.c)
target_compile_options(audiosync PRIVATE
    -std=c11
    -O3
    -march=armv8.2-a+simd+fp16+dotprod+crypto+rcpc+lse
    -mtune=cortex-a76
    -flto=full
    -funroll-loops
    -fomit-frame-pointer
    -fno-plt
    -fno-semantic-interposition
    -fno-common
    -ffp-contract=off
    -fno-stack-protector
    -fno-stack-clash-protection
    -mharden-sls=none
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -fno-exceptions
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -DNDEBUG
)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_options(audiosync PRIVATE
    -flto=full
    -Wl,--gc-sections
    -Wl,-O3
    -Wl,--as-needed
    -Wl,--icf=all
    -Wl,-z,norelro
    -Wl,--strip-all
    -Wl,--build-id=none
)
target_link_libraries(audiosync log aaudio)