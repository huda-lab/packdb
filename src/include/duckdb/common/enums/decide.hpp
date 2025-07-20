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

} // namespace duckdb