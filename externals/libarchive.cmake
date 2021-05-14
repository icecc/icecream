pkg_check_modules(libarchive REQUIRED IMPORTED_TARGET libarchive)
message("Include directory: ${libarchive_INCLUDE_DIRS}")
message("Libraries: ${libarchive_LINK_LIBRARIES}")
