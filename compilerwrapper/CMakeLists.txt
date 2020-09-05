add_executable(compilerwrapper compilerwrapper.cpp)
target_link_libraries(compilerwrapper PRIVATE copts_common)

install(TARGETS compilerwrapper DESTINATION libexec)

