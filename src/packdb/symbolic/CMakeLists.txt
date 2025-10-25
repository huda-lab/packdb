add_library_unity(duckdb_packdb_symbolic OBJECT decide_symbolic.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_packdb_symbolic>
    PARENT_SCOPE)

