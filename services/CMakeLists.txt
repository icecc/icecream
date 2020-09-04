include(CheckIncludeFile)
include(CheckCSourceCompiles)

find_library(zstd NAMES libzstd.a libzstd.so REQUIRED)
find_library(lzo2 NAMES liblzo2.a liblzo2.so REQUIRED)
find_library(dl NAMES libdl.a libdl.so REQUIRED)

check_include_file(ifaddrs.h HAVE_IFADDRS_H)
check_include_file(netinet/tcp_var.h HAVE_NETINET_TCP_VAR_H)


check_c_source_compiles(
    "
    #include <stdlib.h>
    void main()
    {
        double result[3];
        (void)getloadavg(result, 3);
    }
    "
    HAVE_GETLOADAVG)

check_c_source_compiles(
    "
    #include <net/if.h>
    void main()
    {
        struct ifreq ifr;
        void* p = &ifr.ifr_dstaddr;
    }
    "
    HAVE_IFR_DSTADDR)

add_library(icecc_la)
target_sources(icecc_la PRIVATE
    src/services_job.cpp
    src/comm.cpp
    src/exitcode.c
    src/getifaddrs.cpp
    src/ncpus.c
    src/pipes.c
    src/tempfile.c
    src/platform.cpp
    src/ggc.cpp
    src/services_util.cpp
    src/logging.cpp)
target_compile_definitions(icecc_la
    PUBLIC
    $<$<BOOL:${HAVE_IFADDRS_H}>:HAVE_IFADDRS_H>
    PRIVATE
    $<$<BOOL:${HAVE_NETINET_TCP_VAR_H}>:HAVE_NETINET_TCP_VAR_H>
    $<$<BOOL:${HAVE_GETLOADAVG}>:HAVE_GETLOADAVG>
    $<$<BOOL:${HAVE_IFR_DSTADDR}>:HAVE_IFR_DSTADDR>)
target_include_directories(icecc_la
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/include)
target_link_libraries(icecc_la
    PRIVATE copts_common -lzstd -llzo2 -ldl)

if (ICECREAM_LTO)
    set_property(TARGET icecc_la PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()
