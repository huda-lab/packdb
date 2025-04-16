include_directories(../../third_party/fmt/include)
include_directories(../../third_party/fmt)

add_subdirectory(util)

add_library_unity(
  duckdb_packdb
  OBJECT)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_packdb>
    PARENT_SCOPE)