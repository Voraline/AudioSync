cmake_minimum_required(VERSION 3.22.1)
project("audiosync" C)

include(FetchContent)

FetchContent_Declare(
    dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(dr_libs)

add_library(audiosync SHARED native-lib.c)

target_include_directories(audiosync PRIVATE ${dr_libs_SOURCE_DIR})
target_link_libraries(audiosync log aaudio)
