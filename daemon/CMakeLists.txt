include(CheckIncludeFile)
include(FindPkgConfig)

pkg_check_modules(LIBCAP_NG libcap-ng)

check_include_file(arpa/nameser.h HAVE_ARPA_NAMESER_H)
check_include_file(devstat.h HAVE_DEVSTAT_H)
check_include_file(kinfo.h HAVE_KINFO_H)
check_include_file(mach/host_info.h HAVE_MACH_HOST_INFO_H)
check_include_file(resolv.h HAVE_RESOLV_H)
check_include_file(sys/param.h HAVE_SYS_PARAM_H)
check_include_file(sys/signal.h HAVE_SYS_SIGNAL_H)
check_include_file(sys/user.h HAVE_SYS_USER_H)
check_include_file(sys/vfs.h HAVE_SYS_VFS_H)

set(CMAKE_REQUIRED_LIBRARIES archive)

check_c_source_compiles(
    "
    #include <archive.h>
    void main()
    {
        struct archive * a;
        archive_read_support_filter_xz(a);
    }
    "
    HAVE_LIBARCHIVE_XZ)

check_c_source_compiles(
    "
    #include <archive.h>
    void main()
    {
        struct archive * a;
        archive_read_support_filter_zstd(a);
    }
    "
    HAVE_LIBARCHIVE_ZSTD)

unset(CMAKE_REQUIRED_LIBRARIES)

add_executable(iceccd
    src/environment.cpp
    src/file_util.cpp
    src/load.cpp
    src/main.cpp
    src/serve.cpp
    src/workit.cpp)
target_compile_options(iceccd PRIVATE ${LIBCAP_NG_CFLAGS})
target_compile_definitions(iceccd PRIVATE
    $<$<BOOL:${HAVE_ARPA_NAMESER_H}>:HAVE_ARPA_NAMESER_H>
    $<$<BOOL:${HAVE_DEVSTAT_H}>:HAVE_DEVSTAT_H>
    $<$<BOOL:${HAVE_KINFO_H}>:HAVE_KINFO_H>
    $<$<BOOL:${HAVE_MACH_HOST_INFO_H}>:HAVE_MACH_HOST_INFO_H>
    $<$<BOOL:${HAVE_RESOLV_H}>:HAVE_RESOLV_H>
    $<$<BOOL:${HAVE_SYS_PARAM_H}>:HAVE_SYS_PARAM_H>
    $<$<BOOL:${HAVE_SYS_SIGNAL_H}>:HAVE_SYS_SIGNAL_H>
    $<$<BOOL:${HAVE_SYS_USER_H}>:HAVE_SYS_USER_H>
    $<$<BOOL:${HAVE_SYS_VFS_H}>:HAVE_SYS_VFS_H>
    $<$<BOOL:${HAVE_LIBARCHIVE_XZ}>:HAVE_LIBARCHIVE_XZ>
    $<$<BOOL:${HAVE_LIBARCHIVE_ZSTD}>:HAVE_LIBARCHIVE_ZSTD>
    $<${LIBCAP_NG_FOUND}:HAVE_LIBCAP_NG>)
target_link_libraries(iceccd PRIVATE copts_common icecc_la
    $<$<OR:$<BOOL:${HAVE_LIBARCHIVE_XZ}>,$<BOOL:${HAVE_LIBARCHIVE_ZSTD}>>:-larchive>
    ${LIBCAP_NG_LIBRARIES})

if (ICECREAM_LTO)
    set_property(TARGET iceccd PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()

install(TARGETS iceccd DESTINATION bin)

