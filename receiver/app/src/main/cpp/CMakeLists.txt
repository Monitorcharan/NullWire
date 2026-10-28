cmake_minimum_required(VERSION 3.22.1)
project("nullwire_native")

add_library(nullwire_native SHARED
    native-lib.cpp
)

find_library(log-lib log)
find_library(aaudio-lib aaudio)

target_link_libraries(nullwire_native
    ${log-lib}
    ${aaudio-lib}
)

target_compile_options(nullwire_native PRIVATE -Wall -Wextra)
target_link_options(nullwire_native PRIVATE -Wl,-z,max-page-size=16384)
