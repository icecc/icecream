set(SOURCES
    src/job.cpp
    src/comm.cpp
    src/exitcode.cpp
    src/getifaddrs.cpp
    src/logging.cpp
    src/ncpus.c
    src/pipes.cpp
    src/tempfile.c
    src/platform.cpp
    src/gcc.cpp
    src/util.cpp

    include/services/job.h
    include/services/comm.h
    include/services/logging.h
    include/services/exitcode.h
    include/services/getifaddrs.h
    include/services/ncpus.h
    include/services/pipes.h
    include/services/tempfile.h
    include/services/platform.h
    include/services/util.h
    include/services/version.h
)

add_library(library ${SOURCES})
add_library(icecc::lib ALIAS library)

target_link_libraries(library
    PUBLIC
        PkgConfig::lzo2
        PkgConfig::libzstd
        PkgConfig::libcap-ng
        ${CMAKE_DL_LIBS}
)
target_include_directories(library PUBLIC include)