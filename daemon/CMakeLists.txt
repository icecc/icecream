set(SOURCES
    src/environment.cpp
    src/environment.h
    src/file_util.cpp
    src/file_util.h
    src/load.cpp
    src/load.h
    src/main.cpp
    src/serve.cpp
    src/serve.h
    src/workit.cpp
    src/workit.h
    src/daemon.cpp
    src/daemon.hpp
    src/client.cpp
    src/client.hpp
)

add_executable(iceccd ${SOURCES})
target_link_libraries(iceccd
    PRIVATE
        icecc::lib
        PkgConfig::libarchive
        PkgConfig::libcap-ng
)