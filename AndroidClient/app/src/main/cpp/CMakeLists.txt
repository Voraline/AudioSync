cmake_minimum_required(VERSION 3.22.1)
project("audiosync" C)

add_library(audiosync SHARED native-lib.c)

target_link_libraries(audiosync log aaudio)
