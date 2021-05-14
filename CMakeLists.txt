cmake_minimum_required(VERSION 3.15)
project(icecream)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RUNDIR CACHE PATH "\"/usr/local/var/run\"")
set(BINDIR CACHE PATH "\"/usr/local/bin\"")

add_definitions(-DRUNDIR=${RUNDIR})
add_definitions(-DBINDIR=${BINDIR})

#We also need some platfrom specific defines, I know they're ugly but for now we need to reflect autotools environment to allow have both working
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    add_definitions(-DHAVE_MACH_HOST_INFO_H=1)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    add_definitions(-DHAVE_SYS_VFS_H=1)
endif()

include(FindPkgConfig)
include(externals/libzstd.cmake)
include(externals/lzo.cmake)
include(externals/libcap-ng.cmake)
include(externals/libarchive.cmake)

add_subdirectory(services)
add_subdirectory(scheduler)
add_subdirectory(compilerwrapper)
add_subdirectory(daemon)
add_subdirectory(client)