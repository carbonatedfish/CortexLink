#include "cron/cron_parser.h"

#include <cctype>
#include <chrono>
#include <ctime>
#include <sstream>
#include <vector>

namespace cortexlink {
namespace cron {

// ===========================================================================
// Internal helpers
// ===========================================================================

namespace {

// Split a string on whitespace.
std::vector<std::string> SplitFields(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string token;
    while (iss >> token) {
        out.push_back(token);
    }
    return out;
}

// Split a string on `sep`.
std::vector<std::string> Split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

// Convert a string to an integer. Returns true on success.
bool ToInt(const std::string &s, int &out) {
    if (s.empty()) return false;
    for (char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

// Parse a single field token and fill the bitset.
// min_val / max_val define the valid range for this field.
bool ParseField(const std::string &token, int min_val, int max_val,
                std::bitset<60> &bits, std::string *error_out) {
    if (token.empty()) {
        if (error_out) *error_out = "empty field";
        return false;
    }

    int count = max_val - min_val + 1;

    // Helper: set a single value with bounds check.
    auto set_val = [&](int v, const char *ctx) -> bool {
        if (v < min_val || v > max_val) {
            if (error_out) {
                *error_out = std::string("value ") + std::to_string(v)
                           + " out of range [" + std::to_string(min_val)
                           + "," + std::to_string(max_val) + "] in "
                           + ctx;
            }
            return false;
        }
        bits.set(static_cast<size_t>(v - min_val));
        return true;
    };

    // Wildcard
    if (token == "*") {
        for (int i = min_val; i <= max_val; ++i) {
            bits.set(static_cast<size_t>(i - min_val));
        }
        return true;
    }

    // Split on comma for lists
    auto parts = Split(token, ',');
    for (const auto &part : parts) {
        // Check for step syntax:  */N  or  N-M/N  or  N/N
        auto slash_pos = part.find('/');
        int step = 1;
        std::string range_part;
        if (slash_pos != std::string::npos) {
            range_part = part.substr(0, slash_pos);
            std::string step_str = part.substr(slash_pos + 1);
            if (!ToInt(step_str, step) || step <= 0) {
                if (error_out) *error_out = "invalid step: " + step_str;
                return false;
            }
        } else {
            range_part = part;
        }

        // Determine low/high for the range
        int low = min_val, high = max_val;
        if (range_part != "*") {
            auto dash_pos = range_part.find('-');
            if (dash_pos != std::string::npos) {
                // Range: N-M
                std::string lo_str = range_part.substr(0, dash_pos);
                std::string hi_str = range_part.substr(dash_pos + 1);
                if (!ToInt(lo_str, low) || !ToInt(hi_str, high)) {
                    if (error_out) *error_out = "invalid range: " + range_part;
                    return false;
                }
            } else {
                // Single value
                int val = 0;
                if (!ToInt(range_part, val)) {
                    if (error_out) *error_out = "invalid value: " + range_part;
                    return false;
                }
                low = high = val;
            }
        }

        // Bounds check
        if (low < min_val || high > max_val || low > high) {
            if (error_out) {
                *error_out = "range " + std::to_string(low) + "-"
                           + std::to_string(high) + " out of bounds ["
                           + std::to_string(min_val) + ","
                           + std::to_string(max_val) + "]";
            }
            return false;
        }

        // Fill with step
        for (int v = low; v <= high; v += step) {
            bits.set(static_cast<size_t>(v - min_val));
        }
    }

    return true;
}

}  // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

bool Parse(const std::string &expr, CronExpr &out, std::string *error_out) {
    auto fields = SplitFields(expr);
    if (fields.size() != 5) {
        if (error_out) {
            *error_out = "expected 5 fields, got " + std::to_string(fields.size());
        }
        return false;
    }

    // This helper adapts ParseField to work with the CronExpr's member bitsets.
    // CronExpr uses bitset<60> for minute, bitset<24> for hour etc.
    // ParseField expects bitset<60> — fine for minute. For smaller fields we
    // use a temporary bitset<60> then copy the relevant bits.

    auto parse_into = [&](const std::string &token, int min_val, int max_val,
                          std::string &err) -> std::bitset<60> {
        std::bitset<60> bits;
        if (!ParseField(token, min_val, max_val, bits, &err)) {
            return bits;
        }
        return bits;
    };

    std::string err;
    auto min_bits = parse_into(fields[0], 0, 59, err);
    if (!err.empty()) { if (error_out) *error_out = "minute: " + err; return false; }
    for (int i = 0; i < 60; ++i) {
        if (min_bits.test(static_cast<size_t>(i))) out.minute.set(static_cast<size_t>(i));
    }

    auto hour_bits = parse_into(fields[1], 0, 23, err);
    if (!err.empty()) { if (error_out) *error_out = "hour: " + err; return false; }
    for (int i = 0; i < 24; ++i) {
        if (hour_bits.test(static_cast<size_t>(i))) out.hour.set(static_cast<size_t>(i));
    }

    auto dom_bits = parse_into(fields[2], 1, 31, err);
    if (!err.empty()) { if (error_out) *error_out = "day-of-month: " + err; return false; }
    for (int i = 1; i <= 31; ++i) {
        if (dom_bits.test(static_cast<size_t>(i - 1))) out.day_of_month.set(static_cast<size_t>(i - 1));
    }

    auto mon_bits = parse_into(fields[3], 1, 12, err);
    if (!err.empty()) { if (error_out) *error_out = "month: " + err; return false; }
    for (int i = 1; i <= 12; ++i) {
        if (mon_bits.test(static_cast<size_t>(i - 1))) out.month.set(static_cast<size_t>(i - 1));
    }

    auto dow_bits = parse_into(fields[4], 0, 7, err);
    if (!err.empty()) { if (error_out) *error_out = "day-of-week: " + err; return false; }
    for (int i = 0; i <= 7; ++i) {
        if (dow_bits.test(static_cast<size_t>(i))) {
            // Alias: 7 (Sunday) → also set 0 (Sunday)
            if (i == 7) {
                out.day_of_week.set(0);
            } else {
                out.day_of_week.set(static_cast<size_t>(i));
            }
        }
    }

    return true;
}

bool Matches(const CronExpr &expr,
             int minute, int hour, int day_of_month,
             int month, int day_of_week) {
    if (!expr.minute.test(static_cast<size_t>(minute))) return false;
    if (!expr.hour.test(static_cast<size_t>(hour))) return false;
    if (!expr.day_of_month.test(static_cast<size_t>(day_of_month - 1))) return false;
    if (!expr.month.test(static_cast<size_t>(month - 1))) return false;
    if (!expr.day_of_week.test(static_cast<size_t>(day_of_week))) return false;
    return true;
}

void GetCurrentTime(int &minute, int &hour, int &day,
                    int &month, int &day_of_week) {
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&now_t);

    minute = tm.tm_min;
    hour = tm.tm_hour;
    day = tm.tm_mday;
    month = tm.tm_mon + 1;          // tm_mon is 0–11
    day_of_week = tm.tm_wday;       // 0 = Sunday
}

std::optional<std::chrono::seconds> ParseOffset(const std::string &offset,
                                                 std::string *error_out) {
    if (offset.empty()) {
        if (error_out) *error_out = "offset string is empty";
        return std::nullopt;
    }

    long long total_seconds = 0;
    std::string num_buf;

    for (char ch : offset) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            num_buf += ch;
        } else if (ch == 's' || ch == 'm' || ch == 'h' || ch == 'd') {
            if (num_buf.empty()) {
                if (error_out) *error_out = "missing number before unit '" + std::string(1, ch) + "'";
                return std::nullopt;
            }
            int val = 0;
            if (!ToInt(num_buf, val)) {
                if (error_out) *error_out = "invalid number: " + num_buf;
                return std::nullopt;
            }
            num_buf.clear();

            switch (ch) {
            case 's': total_seconds += val; break;
            case 'm': total_seconds += val * 60LL; break;
            case 'h': total_seconds += val * 3600LL; break;
            case 'd': total_seconds += val * 86400LL; break;
            }
        } else {
            if (error_out) *error_out = std::string("unknown unit: '") + ch + "'";
            return std::nullopt;
        }
    }

    if (!num_buf.empty()) {
        if (error_out) *error_out = "trailing number without unit: " + num_buf;
        return std::nullopt;
    }

    if (total_seconds <= 0) {
        if (error_out) *error_out = "offset must be positive";
        return std::nullopt;
    }

    return std::chrono::seconds(total_seconds);
}

std::string MakeCronFromOffset(std::chrono::seconds offset) {
    auto now = std::chrono::system_clock::now();
    auto target = now + offset;
    auto target_t = std::chrono::system_clock::to_time_t(target);
    std::tm tm = *std::localtime(&target_t);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d %d %d %d *",
                  tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon + 1);
    return std::string(buf);
}

}  // namespace cron
}  // namespace cortexlink
