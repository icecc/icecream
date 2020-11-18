pkg_check_modules(libzstd REQUIRED IMPORTED_TARGET libzstd)
message("Include directory: ${libzstd_INCLUDE_DIRS}")
message("Libraries: ${libzstd_LINK_LIBRARIES}")
