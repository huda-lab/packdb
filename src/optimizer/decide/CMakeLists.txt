add_library_unity(
  duckdb_optimizer_decide
  OBJECT
  decide_optimizer.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_optimizer_decide>
    PARENT_SCOPE)
