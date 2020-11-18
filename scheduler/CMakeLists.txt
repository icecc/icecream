set(SOURCES
    src/compileserver.cpp
    src/compileserver.h
    src/job.cpp
    src/job.h
    src/jobstat.cpp
    src/jobstat.h
    src/scheduler.cpp
    src/scheduler.h
)

add_executable(icecc-scheduler ${SOURCES})
target_link_libraries(icecc-scheduler PRIVATE icecc::lib)