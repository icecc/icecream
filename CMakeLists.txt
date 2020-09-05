cmake_minimum_required(VERSION 3.10)

project(icecream VERSION 1.4.90)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

option(ICECREAM_LTO "Build with link-time optimization" OFF)
option(ICECREAM_PIE "Build position-independent executables" ON)

set(ICECREAM_DEBUG_LEVEL 0 CACHE STRING "Debug logging level")
set(ICECREAM_RUNDIR /var/run CACHE STRING "PID file directory")

add_library(copts_pie INTERFACE)
target_compile_options(copts_pie INTERFACE -fpie)
target_link_options(copts_pie INTERFACE -fpie)

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
target_compile_definitions(copts_common INTERFACE
    DEBUG_LEVEL=${ICECREAM_DEBUG_LEVEL}
    BINDIR="${CMAKE_INSTALL_PREFIX}/bin"
    VERSION="${CMAKE_PROJECT_VERSION}"
    RUNDIR="${ICECREAM_RUNDIR}")
target_link_libraries(copts_common INTERFACE
    $<$<BOOL:${ICECREAM_PIE}>:copts_pie>)

add_subdirectory(client)
add_subdirectory(compilerwrapper)
add_subdirectory(create-env)
add_subdirectory(daemon)
add_subdirectory(scheduler)
add_subdirectory(services)
