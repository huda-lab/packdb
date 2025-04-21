add_library_unity(
  duckdb_packdb_naive
  OBJECT
  deterministic_naive.cpp)
target_link_libraries(duckdb_packdb_naive highs::highs)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_packdb_naive>
    PARENT_SCOPE)