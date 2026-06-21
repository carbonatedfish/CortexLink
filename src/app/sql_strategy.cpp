#include "app/sql_strategy.h"

#include <spdlog/spdlog.h>

#include "util/uuid_util.h"

namespace cortexlink {

// ============================================================================
// SqlStrategy — default implementations
// ============================================================================

bool SqlStrategy::ValidateParams(const nlohmann::json & /*params*/) const
{
    return true;
}

bool SqlStrategy::BindUuidParam(sqlite3_stmt *stmt, int idx,
                                const nlohmann::json &params,
                                const std::string &key)
{
    if (!params.contains(key) || !params[key].is_string()) {
        spdlog::warn("SqlStrategy: param '{}' missing or not a string", key);
        return false;
    }

    std::string uuid_str = params[key].get<std::string>();
    std::array<uint8_t, 16> blob = util::UuidToBlob(uuid_str);

    int rc = sqlite3_bind_blob(stmt, idx, blob.data(),
                               static_cast<int>(blob.size()),
                               SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        spdlog::error("SqlStrategy: bind_blob failed at idx {}: {}", idx,
                      sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return false;
    }
    return true;
}

bool SqlStrategy::BindIntParam(sqlite3_stmt *stmt, int idx,
                               const nlohmann::json &params,
                               const std::string &key)
{
    if (!params.contains(key)) {
        spdlog::warn("SqlStrategy: param '{}' missing", key);
        return false;
    }

    const auto &val = params[key];
    sqlite3_int64 int_val = 0;

    if (val.is_number_integer()) {
        int_val = val.get<sqlite3_int64>();
    } else if (val.is_number_float()) {
        int_val = static_cast<sqlite3_int64>(val.get<double>());
    } else if (val.is_string()) {
        try {
            int_val = std::stoll(val.get<std::string>());
        } catch (...) {
            spdlog::warn("SqlStrategy: param '{}' is not a valid integer", key);
            return false;
        }
    } else {
        spdlog::warn("SqlStrategy: param '{}' has unsupported type", key);
        return false;
    }

    int rc = sqlite3_bind_int64(stmt, idx, int_val);
    if (rc != SQLITE_OK) {
        spdlog::error("SqlStrategy: bind_int64 failed at idx {}: {}", idx,
                      sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return false;
    }
    return true;
}

// ============================================================================
// GetDeviceListStrategy
// ============================================================================

std::string GetDeviceListStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM device_property";
}

bool GetDeviceListStrategy::BindParams(sqlite3_stmt * /*stmt*/,
                                       const nlohmann::json & /*params*/) const
{
    return true;
}

// ============================================================================
// GetDeviceDetailStrategy
// ============================================================================

std::string GetDeviceDetailStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM device_property WHERE dev_id = ?";
}

bool GetDeviceDetailStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("dev_id") && params["dev_id"].is_string() &&
           !params["dev_id"].get<std::string>().empty();
}

bool GetDeviceDetailStrategy::BindParams(sqlite3_stmt *stmt,
                                         const nlohmann::json &params) const
{
    return BindUuidParam(stmt, 1, params, "dev_id");
}

// ============================================================================
// GetDeviceDataStrategy
// ============================================================================

std::string GetDeviceDataStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM device_data WHERE dev_id = ?";
}

bool GetDeviceDataStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("dev_id") && params["dev_id"].is_string() &&
           !params["dev_id"].get<std::string>().empty();
}

bool GetDeviceDataStrategy::BindParams(sqlite3_stmt *stmt,
                                       const nlohmann::json &params) const
{
    return BindUuidParam(stmt, 1, params, "dev_id");
}

// ============================================================================
// GetRulesStrategy
// ============================================================================

std::string GetRulesStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM rule";
}

bool GetRulesStrategy::BindParams(sqlite3_stmt * /*stmt*/,
                                  const nlohmann::json & /*params*/) const
{
    return true;
}

// ============================================================================
// GetRuleDetailStrategy
// ============================================================================

std::string GetRuleDetailStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM rule WHERE rule_id = ?";
}

bool GetRuleDetailStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("rule_id");
}

bool GetRuleDetailStrategy::BindParams(sqlite3_stmt *stmt,
                                       const nlohmann::json &params) const
{
    return BindIntParam(stmt, 1, params, "rule_id");
}

// ============================================================================
// GetUserProfilesStrategy
// ============================================================================

std::string GetUserProfilesStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM user_profile";
}

bool GetUserProfilesStrategy::BindParams(sqlite3_stmt * /*stmt*/,
                                         const nlohmann::json & /*params*/) const
{
    return true;
}

}  // namespace cortexlink
