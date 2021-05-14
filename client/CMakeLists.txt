set(LIBRARY_SOURCES
    src/util.h
    src/arg.cpp
    src/argv.c
    src/argv.h
    src/client.h
    src/cpp.cpp
    src/local.cpp
    src/md5.c
    src/md5.h
    src/remote.cpp
    src/safeguard.cpp
    src/util.cpp
)

set(SOURCES src/main.cpp)

add_library(client ${LIBRARY_SOURCES})
target_link_libraries(client
    PRIVATE
        icecc::lib
)
add_library(icecc::client ALIAS client)

add_executable(icecc ${SOURCES})
target_link_libraries(icecc
    PRIVATE
        icecc::client
        icecc::lib
)
