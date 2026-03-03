//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/common/enums/decide.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

enum class DecideSense : uint8_t {
    MAXIMIZE = 0,
    MINIMIZE = 1
};

enum class DecideExpression : uint8_t {
    INVALID = 0,
    VARIABLE,
    SUM
};

enum class DeterministicConstraintSense : uint8_t {
    GTEQ,
    LTEQ,
    EQ
};

//! Tag used to identify WHEN-conditional constraints throughout the pipeline
static constexpr const char *WHEN_CONSTRAINT_TAG = "__when_constraint__";

//! Tag used to identify PER-grouped constraints throughout the pipeline
static constexpr const char *PER_CONSTRAINT_TAG = "__per_constraint__";

} // namespace duckdb