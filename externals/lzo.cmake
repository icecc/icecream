pkg_check_modules(lzo2 REQUIRED IMPORTED_TARGET lzo2)
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    get_filename_component(FOLDER_ABOVE "${lzo2_INCLUDE_DIRS}/.." ABSOLUTE)
    set(lzo2_INCLUDE_DIRS ${FOLDER_ABOVE})
    target_include_directories(PkgConfig::lzo2 INTERFACE ${FOLDER_ABOVE})
endif ()

message("Include directory: ${lzo2_INCLUDE_DIRS}")
message("Libraries: ${lzo2_LINK_LIBRARIES}")
