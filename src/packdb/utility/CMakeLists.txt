add_library_unity(
  duckdb_packdb_util
  OBJECT
  debug.cpp
  ilp_model_builder.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_packdb_util>
    PARENT_SCOPE)