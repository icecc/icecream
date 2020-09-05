configure_file(
    icecc-create-env.in
    icecc-create-env
    @ONLY)
install(PROGRAMS icecc-create-env DESTINATION libexec)

