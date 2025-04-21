include_directories(../../third_party/fmt/include)
include_directories(../../third_party/fmt)

add_subdirectory(utility)
add_subdirectory(naive)

add_library_unity(
  duckdb_packdb
  OBJECT)
target_link_libraries(duckdb_packdb PRIVATE highs::highs)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_packdb>
    PARENT_SCOPE)