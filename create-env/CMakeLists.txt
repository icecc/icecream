configure_file(
    icecc-create-env.in
    icecc-create-env
    @ONLY)
install(PROGRAMS $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/icecc-create-env> DESTINATION libexec)

