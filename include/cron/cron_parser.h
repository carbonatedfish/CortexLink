#pragma once

#include <bitset>
#include <chrono>
#include <optional>
#include <string>

namespace cortexlink {
namespace cron {

// Parsed representation of a 5-field cron expression.
// Each bitset index corresponds to a valid value for that field.
struct CronExpr {
    std::bitset<60> minute;         // index 0..59
    std::bitset<24> hour;           // index 0..23
    std::bitset<31> day_of_month;   // index 0 = day 1, ..., 30 = day 31
    std::bitset<12> month;          // index 0 = January, ..., 11 = December
    std::bitset<8>  day_of_week;    // index 0 = Sunday, ..., 6 = Saturday; 7 aliases 0

    CronExpr() {
        minute.reset();
        hour.reset();
        day_of_month.reset();
        month.reset();
        day_of_week.reset();
    }
};

// Parse a standard 5-field cron expression.
//
// Fields: minute hour day-of-month month day-of-week
// Ranges: 0-59  0-23  1-31         1-12   0-6 (0=Sunday)
//
// Supported syntax per field:  *  wildcard
//                              5  single value
//                            1-5  inclusive range
//                            */5  every 5th value (step)
//                         1,3,5  comma-separated list
//                         1-5/2  range with step
//
// Returns true on success. On failure, *error_out (if non-null) receives a
// human-readable message.
bool Parse(const std::string &expr, CronExpr &out, std::string *error_out = nullptr);

// Check whether the given wall-clock time matches a parsed cron expression.
//
// All values use natural numbering:
//   minute        0–59
//   hour          0–23
//   day_of_month  1–31
//   month         1–12
//   day_of_week   0–6  (0 = Sunday)
bool Matches(const CronExpr &expr,
             int minute, int hour, int day_of_month,
             int month, int day_of_week);

// Fill the parameters with current wall-clock time (UTC+8) components.
// Uses std::localtime — assumes the system timezone is set to Asia/Shanghai.
void GetCurrentTime(int &minute, int &hour, int &day,
                    int &month, int &day_of_week);

// Parse a human-readable time offset string into a chrono duration.
//
// Supported units:  s = seconds   m = minutes   h = hours   d = days
// Examples:  "30m"  "2h"  "1d"  "1h30m"  "2d6h"  "90s"
//
// Units may appear in any order and may be repeated. Returns std::nullopt
// on parse error; *error_out receives a description.
std::optional<std::chrono::seconds> ParseOffset(const std::string &offset,
                                                 std::string *error_out = nullptr);

// Compute an absolute cron expression that matches once at
//   now + offset (UTC+8).
//
// The resulting expression pins minute, hour, day-of-month, and month so
// that the cron fires exactly at the target wall-clock time. day-of-week
// is set to * (wildcard).
//
// Intended for use by add_relative_cron.
std::string MakeCronFromOffset(std::chrono::seconds offset);

}  // namespace cron
}  // namespace cortexlink
