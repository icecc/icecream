include(CheckCSourceCompiles)

check_c_source_compiles(
    "
    #include <string.h>
    void main()
    {
        char* result = strsignal(0);
    }
    "
    HAVE_STRSIGNAL)

check_c_source_compiles(
    "
    #include <sys/file.h>
    void main()
    {
        int result = flock(-1, LOCK_UN);
    }
    "
    HAVE_FLOCK)

check_c_source_compiles(
    "
    #include <unistd.h>
    void main()
    {
        int result = lockf(-1, F_TEST, 0);
    }
    "
    HAVE_LOCKF)

add_library(client)
target_sources(client PRIVATE
    src/arg.cpp
    src/argv.c
    src/call_cpp.cpp
    src/client_util.cpp
    src/local.cpp
    src/main.cpp
    src/md5.c
    src/remote.cpp
    src/safeguard.cpp)
    target_compile_definitions(client PRIVATE
    $<$<BOOL:${HAVE_STRSIGNAL}>:HAVE_STRSIGNAL>
    $<$<BOOL:${HAVE_FLOCK}>:HAVE_FLOCK>
    $<$<BOOL:${HAVE_LOCKF}>:HAVE_LOCKF>)
target_link_libraries(client PRIVATE copts_common icecc_la)

add_executable(icecc src/main.cpp)
target_link_libraries(icecc PRIVATE copts_common client icecc_la)

if (ICECREAM_LTO)
    set_property(TARGET client PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()

install(TARGETS icecc DESTINATION bin)

