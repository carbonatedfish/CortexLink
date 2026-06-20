#include "util/log_util.h"

#include <memory>

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace cortexlink {
namespace util {

void InitLogger(const std::string &log_dir,
                spdlog::level::level_enum level)
{
    // Build the daily file path pattern (spdlog appends _YYYY-MM-DD)
    std::string daily_path = log_dir + "/cortexlink.log";

    // Create sinks
    auto daily_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
        daily_path, 0, 0);  // rotate at 00:00
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    // Set pattern on each sink
    daily_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
    stdout_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    // Combine sinks into one logger
    spdlog::sinks_init_list sink_list = {stdout_sink, daily_sink};
    auto logger = std::make_shared<spdlog::logger>("cortexlink", sink_list);
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);

    // Register as the global default logger
    spdlog::set_default_logger(logger);
}

}  // namespace util
}  // namespace cortexlink
