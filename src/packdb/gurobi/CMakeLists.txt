add_library_unity(
  duckdb_packdb_gurobi
  OBJECT
  gurobi_solver.cpp)

if(PACKDB_GUROBI_FOUND)
  target_compile_definitions(duckdb_packdb_gurobi PRIVATE PACKDB_HAS_GUROBI=1)
  target_include_directories(duckdb_packdb_gurobi PRIVATE ${GUROBI_INCLUDE_DIR})
else()
  target_compile_definitions(duckdb_packdb_gurobi PRIVATE PACKDB_HAS_GUROBI=0)
endif()

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_packdb_gurobi>
    PARENT_SCOPE)
