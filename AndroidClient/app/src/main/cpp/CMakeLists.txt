cmake_minimum_required(VERSION 3.22.1)
project("audiosync" C)

add_library(audiosync SHARED native-lib.c)

target_compile_options(audiosync PRIVATE
    -std=c11
    -O3
    -march=armv8.2-a+simd
    -mtune=cortex-a76
    -flto=thin
    -funroll-loops
    -fomit-frame-pointer
    -fno-plt
    -fno-semantic-interposition
    -fno-common
    -fno-math-errno
    -ffp-contract=on
    -fno-stack-protector
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -fno-exceptions
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -DNDEBUG
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
)

target_link_options(audiosync PRIVATE
    -flto=thin
    -Wl,--gc-sections
    -Wl,-O3
    -Wl,--as-needed
    -Wl,--icf=all
    -Wl,-z,norelro
    -Wl,-z,noseparate-code
    -Wl,--build-id=none
)

target_link_libraries(audiosync log aaudio)
