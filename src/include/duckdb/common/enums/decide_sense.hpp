//===----------------------------------------------------------------------===//
//                         PackDB
//
// duckdb/common/enums/decide_sense.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

enum class DecideSense : uint8_t {
    MAXIMIZE = 0,
    MINIMIZE = 1
};

} // namespace duckdb