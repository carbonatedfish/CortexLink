#include "llm/llm_sql_strategy.h"

#include <spdlog/spdlog.h>

#include "util/uuid_util.h"

namespace cortexlink {

// ============================================================================
// LlmSqlStrategy — default implementations
// ============================================================================

bool LlmSqlStrategy::ValidateParams(const nlohmann::json & /*params*/) const
{
    return true;
}

bool LlmSqlStrategy::IsWrite() const
{
    return false;
}

nlohmann::json LlmSqlStrategy::PostExecute(sqlite3 * /*db*/) const
{
    return nlohmann::json::object();
}

bool LlmSqlStrategy::BindUuidParam(sqlite3_stmt *stmt, int idx,
                                   const nlohmann::json &params,
                                   const std::string &key)
{
    if (!params.contains(key) || !params[key].is_string()) {
        spdlog::warn("LlmSqlStrategy: param '{}' missing or not a string", key);
        return false;
    }

    std::string uuid_str = params[key].get<std::string>();
    std::array<uint8_t, 16> blob = util::UuidToBlob(uuid_str);

    int rc = sqlite3_bind_blob(stmt, idx, blob.data(),
                               static_cast<int>(blob.size()),
                               SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        spdlog::error("LlmSqlStrategy: bind_blob failed at idx {}: {}", idx,
                      sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return false;
    }
    return true;
}

bool LlmSqlStrategy::BindIntParam(sqlite3_stmt *stmt, int idx,
                                  const nlohmann::json &params,
                                  const std::string &key)
{
    if (!params.contains(key)) {
        spdlog::warn("LlmSqlStrategy: param '{}' missing", key);
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
            spdlog::warn("LlmSqlStrategy: param '{}' is not a valid integer", key);
            return false;
        }
    } else {
        spdlog::warn("LlmSqlStrategy: param '{}' has unsupported type", key);
        return false;
    }

    int rc = sqlite3_bind_int64(stmt, idx, int_val);
    if (rc != SQLITE_OK) {
        spdlog::error("LlmSqlStrategy: bind_int64 failed at idx {}: {}", idx,
                      sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return false;
    }
    return true;
}

bool LlmSqlStrategy::BindTextParam(sqlite3_stmt *stmt, int idx,
                                   const nlohmann::json &params,
                                   const std::string &key)
{
    if (!params.contains(key) || !params[key].is_string()) {
        spdlog::warn("LlmSqlStrategy: param '{}' missing or not a string", key);
        return false;
    }

    std::string text = params[key].get<std::string>();
    int rc = sqlite3_bind_text(stmt, idx, text.c_str(),
                               static_cast<int>(text.size()),
                               SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        spdlog::error("LlmSqlStrategy: bind_text failed at idx {}: {}", idx,
                      sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return false;
    }
    return true;
}

bool LlmSqlStrategy::BindBoolParam(sqlite3_stmt *stmt, int idx,
                                   const nlohmann::json &params,
                                   const std::string &key)
{
    if (!params.contains(key)) {
        spdlog::warn("LlmSqlStrategy: param '{}' missing", key);
        return false;
    }

    int val = params[key].get<bool>() ? 1 : 0;
    int rc = sqlite3_bind_int(stmt, idx, val);
    if (rc != SQLITE_OK) {
        spdlog::error("LlmSqlStrategy: bind_int failed at idx {}: {}", idx,
                      sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return false;
    }
    return true;
}

// ============================================================================
// LlmCmdRouter
// ============================================================================

LlmCmdRouter::LlmCmdRouter()
{
    Register("get_device_properties",
             std::make_unique<LlmGetDevicePropertiesStrategy>());
    Register("get_device_property",
             std::make_unique<LlmGetDevicePropertyStrategy>());
    Register("get_events",
             std::make_unique<LlmGetEventsStrategy>());
    Register("get_event",
             std::make_unique<LlmGetEventStrategy>());
    Register("get_rules",
             std::make_unique<LlmGetRulesStrategy>());
    Register("get_rule",
             std::make_unique<LlmGetRuleStrategy>());
    Register("get_event_records",
             std::make_unique<LlmGetEventRecordsStrategy>());
    Register("get_event_records_by_device",
             std::make_unique<LlmGetEventRecordsByDeviceStrategy>());
    Register("get_event_records_by_time",
             std::make_unique<LlmGetEventRecordsByTimeStrategy>());
    Register("insert_rule",
             std::make_unique<LlmInsertRuleStrategy>());
    Register("update_rule",
             std::make_unique<LlmUpdateRuleStrategy>());
    Register("delete_rule",
             std::make_unique<LlmDeleteRuleStrategy>());
    Register("set_rule_enable",
             std::make_unique<LlmSetRuleEnableStrategy>());
}

LlmSqlStrategy *LlmCmdRouter::Lookup(const std::string &cmd) const
{
    auto it = strategies_.find(cmd);
    if (it == strategies_.end()) {
        return nullptr;
    }
    return it->second.get();
}

void LlmCmdRouter::Register(const std::string &cmd,
                            std::unique_ptr<LlmSqlStrategy> strategy)
{
    strategies_[cmd] = std::move(strategy);
}

// ============================================================================
// LlmGetDevicePropertiesStrategy
// ============================================================================

std::string LlmGetDevicePropertiesStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM device_property";
}

bool LlmGetDevicePropertiesStrategy::BindParams(
    sqlite3_stmt * /*stmt*/, const nlohmann::json & /*params*/) const
{
    return true;
}

// ============================================================================
// LlmGetDevicePropertyStrategy
// ============================================================================

std::string LlmGetDevicePropertyStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM device_property WHERE dev_id = ?";
}

bool LlmGetDevicePropertyStrategy::ValidateParams(
    const nlohmann::json &params) const
{
    return params.contains("dev_id") && params["dev_id"].is_string() &&
           !params["dev_id"].get<std::string>().empty();
}

bool LlmGetDevicePropertyStrategy::BindParams(
    sqlite3_stmt *stmt, const nlohmann::json &params) const
{
    return BindUuidParam(stmt, 1, params, "dev_id");
}

// ============================================================================
// LlmGetEventsStrategy
// ============================================================================

std::string LlmGetEventsStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM event";
}

bool LlmGetEventsStrategy::BindParams(
    sqlite3_stmt * /*stmt*/, const nlohmann::json & /*params*/) const
{
    return true;
}

// ============================================================================
// LlmGetEventStrategy
// ============================================================================

std::string LlmGetEventStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM event WHERE evt_id = ?";
}

bool LlmGetEventStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("evt_id") && params["evt_id"].is_string() &&
           !params["evt_id"].get<std::string>().empty();
}

bool LlmGetEventStrategy::BindParams(sqlite3_stmt *stmt,
                                     const nlohmann::json &params) const
{
    return BindUuidParam(stmt, 1, params, "evt_id");
}

// ============================================================================
// LlmGetRulesStrategy
// ============================================================================

std::string LlmGetRulesStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM rule";
}

bool LlmGetRulesStrategy::BindParams(
    sqlite3_stmt * /*stmt*/, const nlohmann::json & /*params*/) const
{
    return true;
}

// ============================================================================
// LlmGetRuleStrategy
// ============================================================================

std::string LlmGetRuleStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM rule WHERE rule_id = ?";
}

bool LlmGetRuleStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("rule_id");
}

bool LlmGetRuleStrategy::BindParams(sqlite3_stmt *stmt,
                                    const nlohmann::json &params) const
{
    return BindIntParam(stmt, 1, params, "rule_id");
}

// ============================================================================
// LlmGetEventRecordsStrategy
// ============================================================================

std::string LlmGetEventRecordsStrategy::GetSql(
    const nlohmann::json &params) const
{
    int limit = params.value("limit", 100);
    return "SELECT * FROM event_record ORDER BY time DESC LIMIT " +
           std::to_string(limit);
}

bool LlmGetEventRecordsStrategy::BindParams(
    sqlite3_stmt * /*stmt*/, const nlohmann::json & /*params*/) const
{
    return true;
}

// ============================================================================
// LlmGetEventRecordsByDeviceStrategy
// ============================================================================

std::string LlmGetEventRecordsByDeviceStrategy::GetSql(
    const nlohmann::json &params) const
{
    int limit = params.value("limit", 100);
    return "SELECT * FROM event_record WHERE dev_id = ? "
           "ORDER BY time DESC LIMIT " +
           std::to_string(limit);
}

bool LlmGetEventRecordsByDeviceStrategy::ValidateParams(
    const nlohmann::json &params) const
{
    return params.contains("dev_id") && params["dev_id"].is_string() &&
           !params["dev_id"].get<std::string>().empty();
}

bool LlmGetEventRecordsByDeviceStrategy::BindParams(
    sqlite3_stmt *stmt, const nlohmann::json &params) const
{
    return BindUuidParam(stmt, 1, params, "dev_id");
}

// ============================================================================
// LlmGetEventRecordsByTimeStrategy
// ============================================================================

std::string LlmGetEventRecordsByTimeStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "SELECT * FROM event_record "
           "WHERE time >= ? AND time <= ? "
           "ORDER BY time DESC";
}

bool LlmGetEventRecordsByTimeStrategy::ValidateParams(
    const nlohmann::json &params) const
{
    return params.contains("start") && params["start"].is_string() &&
           !params["start"].get<std::string>().empty() &&
           params.contains("end") && params["end"].is_string() &&
           !params["end"].get<std::string>().empty();
}

bool LlmGetEventRecordsByTimeStrategy::BindParams(
    sqlite3_stmt *stmt, const nlohmann::json &params) const
{
    if (!BindTextParam(stmt, 1, params, "start")) return false;
    return BindTextParam(stmt, 2, params, "end");
}

// ============================================================================
// LlmInsertRuleStrategy
// ============================================================================

std::string LlmInsertRuleStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return R"(INSERT INTO rule (rule_name, rule_type, enable, count, "limit", )"
           R"(cond_expr, action) VALUES (?, ?, ?, ?, ?, ?, ?))";
}

bool LlmInsertRuleStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("rule_name") && params["rule_name"].is_string() &&
           !params["rule_name"].get<std::string>().empty() &&
           params.contains("rule_type") && params["rule_type"].is_string() &&
           !params["rule_type"].get<std::string>().empty() &&
           params.contains("action") && params["action"].is_string() &&
           !params["action"].get<std::string>().empty();
}

bool LlmInsertRuleStrategy::BindParams(sqlite3_stmt *stmt,
                                       const nlohmann::json &params) const
{
    // 1: rule_name (required)
    std::string rule_name = params["rule_name"].get<std::string>();
    sqlite3_bind_text(stmt, 1, rule_name.c_str(),
                      static_cast<int>(rule_name.size()), SQLITE_TRANSIENT);

    // 2: rule_type (required)
    std::string rule_type = params["rule_type"].get<std::string>();
    sqlite3_bind_text(stmt, 2, rule_type.c_str(),
                      static_cast<int>(rule_type.size()), SQLITE_TRANSIENT);

    // 3: enable (optional, default true)
    int enable = params.value("enable", true) ? 1 : 0;
    sqlite3_bind_int(stmt, 3, enable);

    // 4: count (optional, default 0)
    sqlite3_bind_int64(stmt, 4, params.value("count", 0));

    // 5: limit (optional, default 0)
    sqlite3_bind_int64(stmt, 5, params.value("limit", 0));

    // 6: cond_expr (optional, default empty)
    std::string cond_expr = params.value("cond_expr", "");
    sqlite3_bind_text(stmt, 6, cond_expr.c_str(),
                      static_cast<int>(cond_expr.size()), SQLITE_TRANSIENT);

    // 7: action (required)
    std::string action = params["action"].get<std::string>();
    sqlite3_bind_text(stmt, 7, action.c_str(),
                      static_cast<int>(action.size()), SQLITE_TRANSIENT);

    return true;
}

bool LlmInsertRuleStrategy::IsWrite() const
{
    return true;
}

nlohmann::json LlmInsertRuleStrategy::PostExecute(sqlite3 *db) const
{
    return nlohmann::json::object({{"rule_id", sqlite3_last_insert_rowid(db)}});
}

// ============================================================================
// LlmUpdateRuleStrategy
// ============================================================================

std::string LlmUpdateRuleStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return R"(UPDATE rule SET rule_name = ?, rule_type = ?, enable = ?, )"
           R"(count = ?, "limit" = ?, cond_expr = ?, action = ? )"
           R"(WHERE rule_id = ?)";
}

bool LlmUpdateRuleStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("rule_id") &&
           params.contains("rule_name") && params["rule_name"].is_string() &&
           params.contains("rule_type") && params["rule_type"].is_string() &&
           params.contains("action") && params["action"].is_string();
}

bool LlmUpdateRuleStrategy::BindParams(sqlite3_stmt *stmt,
                                       const nlohmann::json &params) const
{
    // 1: rule_name
    std::string rule_name = params["rule_name"].get<std::string>();
    sqlite3_bind_text(stmt, 1, rule_name.c_str(),
                      static_cast<int>(rule_name.size()), SQLITE_TRANSIENT);

    // 2: rule_type
    std::string rule_type = params["rule_type"].get<std::string>();
    sqlite3_bind_text(stmt, 2, rule_type.c_str(),
                      static_cast<int>(rule_type.size()), SQLITE_TRANSIENT);

    // 3: enable
    int enable = params.value("enable", true) ? 1 : 0;
    sqlite3_bind_int(stmt, 3, enable);

    // 4: count
    sqlite3_bind_int64(stmt, 4, params.value("count", 0));

    // 5: limit
    sqlite3_bind_int64(stmt, 5, params.value("limit", 0));

    // 6: cond_expr
    std::string cond_expr = params.value("cond_expr", "");
    sqlite3_bind_text(stmt, 6, cond_expr.c_str(),
                      static_cast<int>(cond_expr.size()), SQLITE_TRANSIENT);

    // 7: action
    std::string action = params["action"].get<std::string>();
    sqlite3_bind_text(stmt, 7, action.c_str(),
                      static_cast<int>(action.size()), SQLITE_TRANSIENT);

    // 8: rule_id
    return BindIntParam(stmt, 8, params, "rule_id");
}

bool LlmUpdateRuleStrategy::IsWrite() const
{
    return true;
}

// ============================================================================
// LlmDeleteRuleStrategy
// ============================================================================

std::string LlmDeleteRuleStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "DELETE FROM rule WHERE rule_id = ?";
}

bool LlmDeleteRuleStrategy::ValidateParams(const nlohmann::json &params) const
{
    return params.contains("rule_id");
}

bool LlmDeleteRuleStrategy::BindParams(sqlite3_stmt *stmt,
                                       const nlohmann::json &params) const
{
    return BindIntParam(stmt, 1, params, "rule_id");
}

bool LlmDeleteRuleStrategy::IsWrite() const
{
    return true;
}

// ============================================================================
// LlmSetRuleEnableStrategy
// ============================================================================

std::string LlmSetRuleEnableStrategy::GetSql(
    const nlohmann::json & /*params*/) const
{
    return "UPDATE rule SET enable = ? WHERE rule_id = ?";
}

bool LlmSetRuleEnableStrategy::ValidateParams(
    const nlohmann::json &params) const
{
    return params.contains("rule_id") && params.contains("enable");
}

bool LlmSetRuleEnableStrategy::BindParams(sqlite3_stmt *stmt,
                                          const nlohmann::json &params) const
{
    if (!BindBoolParam(stmt, 1, params, "enable")) return false;
    return BindIntParam(stmt, 2, params, "rule_id");
}

bool LlmSetRuleEnableStrategy::IsWrite() const
{
    return true;
}

}  // namespace cortexlink
