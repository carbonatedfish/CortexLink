#include "util/time_util.h"

#include <chrono>
#include <ctime>

namespace cortexlink {
namespace util {

std::string CurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    // UTC+8 (East-8 timezone, matching RuleEngine::GetCurrentTime)
    now_time_t += 8 * 3600;

    std::tm utc8_tm;
    gmtime_r(&now_time_t, &utc8_tm);

    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &utc8_tm);
    return std::string(buf);
}

}  // namespace util
}  // namespace cortexlink
