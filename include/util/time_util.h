#pragma once

#include <string>

namespace cortexlink {
namespace util {

// Returns current UTC+8 (East-8) timestamp as "YYYY-MM-DD HH:MM:SS".
std::string CurrentTimestamp();

}  // namespace util
}  // namespace cortexlink
