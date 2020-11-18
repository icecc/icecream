pkg_check_modules(lzo2 REQUIRED IMPORTED_TARGET lzo2)
message("Include directory: ${lzo2_INCLUDE_DIRS}")
message("Libraries: ${lzo2_LINK_LIBRARIES}")
