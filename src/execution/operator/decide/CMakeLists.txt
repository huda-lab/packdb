add_library_unity(duckdb_operator_decide OBJECT physical_decide.cpp)
target_link_libraries(duckdb_operator_decide highs::highs)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_operator_decide>
    PARENT_SCOPE)
