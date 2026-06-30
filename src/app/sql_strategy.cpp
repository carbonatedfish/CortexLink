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
    spdlog::debug("SqlStrategy: bound uuid param '{}' at idx {} value='{}'",
                  key, idx, uuid_str);
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
    spdlog::debug("SqlStrategy: bound int param '{}' at idx {} value={}",
                  key, idx, int_val);
    return true;
}

bool SqlStrategy::BindTextParam(sqlite3_stmt *stmt, int idx,
                                const nlohmann::json &params,
                                const std::string &key)
{
    if (!params.contains(key) || !params[key].is_string()) {
        spdlog::warn("SqlStrategy: param '{}' missing or not a string", key);
        return false;
    }

    std::string text = params[key].get<std::string>();
    int rc = sqlite3_bind_text(stmt, idx, text.c_str(),
                               static_cast<int>(text.size()),
                               SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        spdlog::error("SqlStrategy: bind_text failed at idx {}: {}", idx,
                      sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return false;
    }
    spdlog::debug("SqlStrategy: bound text param '{}' at idx {} value='{}'",
                  key, idx, text);
    return true;
}

bool SqlStrategy::IsWrite() const
{
    return false;
}

nlohmann::json SqlStrategy::PostExecute(sqlite3 * /*db*/) const
{
    return nlohmann::json::object();
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

// ============================================================================
// InsertUserProfileStrategy
// ============================================================================

std::string InsertUserProfileStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "INSERT INTO user_profile (user_id, user_name, preference) "
           "VALUES (?, ?, ?)";
}

bool InsertUserProfileStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("user_id") && params["user_id"].is_string() &&
           !params["user_id"].get<std::string>().empty() &&
           params.contains("user_name") && params["user_name"].is_string() &&
           !params["user_name"].get<std::string>().empty();
}

bool InsertUserProfileStrategy::BindParams(sqlite3_stmt *stmt,
                                           const nlohmann::json &params) const
{
    if (!BindUuidParam(stmt, 1, params, "user_id")) return false;
    if (!BindTextParam(stmt, 2, params, "user_name")) return false;

    // preference is optional — bind empty string when absent
    if (params.contains("preference") && params["preference"].is_string()) {
        std::string pref = params["preference"].get<std::string>();
        int rc = sqlite3_bind_text(stmt, 3, pref.c_str(),
                                   static_cast<int>(pref.size()),
                                   SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            spdlog::error("SqlStrategy: bind_text failed at idx 3: {}",
                          sqlite3_errmsg(sqlite3_db_handle(stmt)));
            return false;
        }
    } else {
        int rc = sqlite3_bind_text(stmt, 3, "", 0, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            spdlog::error("SqlStrategy: bind_text failed at idx 3: {}",
                          sqlite3_errmsg(sqlite3_db_handle(stmt)));
            return false;
        }
    }
    return true;
}

bool InsertUserProfileStrategy::IsWrite() const
{
    return true;
}

// ============================================================================
// UpdateUserProfileStrategy
// ============================================================================

std::string UpdateUserProfileStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "UPDATE user_profile SET user_name = ?, preference = ? "
           "WHERE user_id = ?";
}

bool UpdateUserProfileStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("user_id") && params["user_id"].is_string() &&
           !params["user_id"].get<std::string>().empty() &&
           params.contains("user_name") && params["user_name"].is_string() &&
           !params["user_name"].get<std::string>().empty();
}

bool UpdateUserProfileStrategy::BindParams(sqlite3_stmt *stmt,
                                           const nlohmann::json &params) const
{
    if (!BindTextParam(stmt, 1, params, "user_name")) return false;

    // preference is optional — bind empty string when absent
    if (params.contains("preference") && params["preference"].is_string()) {
        std::string pref = params["preference"].get<std::string>();
        int rc = sqlite3_bind_text(stmt, 2, pref.c_str(),
                                   static_cast<int>(pref.size()),
                                   SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            spdlog::error("SqlStrategy: bind_text failed at idx 2: {}",
                          sqlite3_errmsg(sqlite3_db_handle(stmt)));
            return false;
        }
    } else {
        int rc = sqlite3_bind_text(stmt, 2, "", 0, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            spdlog::error("SqlStrategy: bind_text failed at idx 2: {}",
                          sqlite3_errmsg(sqlite3_db_handle(stmt)));
            return false;
        }
    }

    if (!BindUuidParam(stmt, 3, params, "user_id")) return false;
    return true;
}

bool UpdateUserProfileStrategy::IsWrite() const
{
    return true;
}

// ============================================================================
// DeleteUserProfileStrategy
// ============================================================================

std::string DeleteUserProfileStrategy::GetSql(const nlohmann::json & /*params*/) const
{
    return "DELETE FROM user_profile WHERE user_id = ?";
}

bool DeleteUserProfileStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("user_id") && params["user_id"].is_string() &&
           !params["user_id"].get<std::string>().empty();
}

bool DeleteUserProfileStrategy::BindParams(sqlite3_stmt *stmt,
                                           const nlohmann::json &params) const
{
    return BindUuidParam(stmt, 1, params, "user_id");
}

bool DeleteUserProfileStrategy::IsWrite() const
{
    return true;
}

}  // namespace cortexlink
