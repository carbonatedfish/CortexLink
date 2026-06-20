#pragma once

#include <string>

#include <spdlog/spdlog.h>

namespace cortexlink {
namespace util {

// Initialize the global default logger with a daily rotating file sink
// and a stdout color sink.  The log directory is created automatically
// if it does not exist.
//
// log_dir  — directory where daily log files are written
//            (a file named "cortexlink_YYYY-MM-DD.log" is created each day)
// level    — minimum log level (default: spdlog::level::info)
void InitLogger(const std::string &log_dir,
                spdlog::level::level_enum level = spdlog::level::info);

}  // namespace util
}  // namespace cortexlink
