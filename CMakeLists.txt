cmake_minimum_required(VERSION 3.10)
project(icecream VERSION 1.3.90)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

add_library(copts_common INTERFACE)
target_compile_options(copts_common INTERFACE
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
