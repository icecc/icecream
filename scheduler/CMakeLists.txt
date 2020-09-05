include(CheckIncludeFile)

check_include_file(sys/signal.h HAVE_SYS_SIGNAL_H)

add_executable(icecc-scheduler
    src/compileserver.cpp
    src/jobstat.cpp
    src/main.cpp
    src/scheduler_job.cpp)
    target_compile_definitions(icecc-scheduler PRIVATE
    $<$<BOOL:${HAVE_SYS_SIGNAL_H}>:HAVE_SYS_SIGNAL_H>)
target_link_libraries(icecc-scheduler PRIVATE copts_common icecc_la)

if (ICECREAM_LTO)
    set_property(TARGET icecc-scheduler PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()

install(TARGETS icecc-scheduler DESTINATION bin)
