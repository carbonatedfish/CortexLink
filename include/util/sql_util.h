#pragma once

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace cortexlink {
namespace util {

// Convert one SQLite result row into a JSON object.
// Column names become keys; 16-byte BLOBs are converted to UUID strings.
nlohmann::json RowToJson(sqlite3_stmt *stmt);

}  // namespace util
}  // namespace cortexlink
