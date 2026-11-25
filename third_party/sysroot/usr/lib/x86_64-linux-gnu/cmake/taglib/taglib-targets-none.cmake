#----------------------------------------------------------------
# Generated CMake target import file for configuration "None".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "TagLib::tag" for configuration "None"
set_property(TARGET TagLib::tag APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(TagLib::tag PROPERTIES
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libtag.so.2.0.2"
  IMPORTED_SONAME_NONE "libtag.so.2"
  )

list(APPEND _cmake_import_check_targets TagLib::tag )
list(APPEND _cmake_import_check_files_for_TagLib::tag "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libtag.so.2.0.2" )

# Import target "TagLib::tag_c" for configuration "None"
set_property(TARGET TagLib::tag_c APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(TagLib::tag_c PROPERTIES
  IMPORTED_LINK_DEPENDENT_LIBRARIES_NONE "TagLib::tag"
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libtag_c.so.2.0.2"
  IMPORTED_SONAME_NONE "libtag_c.so.2"
  )

list(APPEND _cmake_import_check_targets TagLib::tag_c )
list(APPEND _cmake_import_check_files_for_TagLib::tag_c "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libtag_c.so.2.0.2" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
