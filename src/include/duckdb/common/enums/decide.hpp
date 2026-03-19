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

//! Type of aggregate used in MIN/MAX objective linearization
enum class ObjectiveAggregateType : uint8_t {
    NONE = 0,   //! No MIN/MAX objective (pure SUM or no objective)
    SUM,        //! SUM aggregate
    MIN_AGG,    //! MIN aggregate (suffixed to avoid MIN/MAX macro collision)
    MAX_AGG     //! MAX aggregate
};

//! Tag used to identify WHEN-conditional constraints throughout the pipeline
static constexpr const char *WHEN_CONSTRAINT_TAG = "__when_constraint__";

//! Tag used to identify PER-grouped constraints throughout the pipeline
static constexpr const char *PER_CONSTRAINT_TAG = "__per_constraint__";

//! Tag used to identify AVG→SUM rewritten aggregates (RHS needs scaling at execution)
static constexpr const char *AVG_REWRITE_TAG = "__avg_rewrite__";

//! Tag prefix for MIN/MAX hard-case indicator linking (on BoundAggregateExpression.alias)
//! Format: "__minmax_ind_<indicator_idx>_<min|max>__"
static constexpr const char *MINMAX_INDICATOR_TAG_PREFIX = "__minmax_ind_";

//! Tag prefix for not-equal indicator linking (on BoundComparisonExpression.alias)
//! Format: "__ne_ind_tag_<indicator_idx>__"
static constexpr const char *NE_INDICATOR_TAG_PREFIX = "__ne_ind_tag_";

} // namespace duckdb