cmake_minimum_required(VERSION 3.10)

project(icecream VERSION 1.4.90)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

option(ICECREAM_LTO "Build with link-time optimization" ON)

add_library(copts_common INTERFACE)
target_compile_options(copts_common INTERFACE
    -fvisibility=hidden
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>
    -Wall
    -Wextra
    -Wpedantic
    -Wwrite-strings
    -Wundef
    -Wpointer-arith
    -Werror)

add_library(copts_pic INTERFACE)
target_compile_options(copts_pic INTERFACE -fPIC)
target_link_options(copts_pic INTERFACE -fPIC)

add_compile_definitions(
    BINDIR="${CMAKE_INSTALL_PATH}"
    VERSION="${CMAKE_PROJECT_VERSION}"
    RUNDIR="/var/run")

add_subdirectory(compilerwrapper)
add_subdirectory(client)
add_subdirectory(daemon)
add_subdirectory(scheduler)
add_subdirectory(services)
