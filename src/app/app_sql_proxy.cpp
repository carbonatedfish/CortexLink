#include "app/app_sql_proxy.h"

#include <chrono>
#include <ctime>
#include <vector>

#include <spdlog/spdlog.h>

#include "util/uuid_util.h"

namespace cortexlink {

// ============================================================================
// CmdRouter
// ============================================================================

CmdRouter::CmdRouter()
{
    Register("get_device_list",
             std::make_unique<GetDeviceListStrategy>());
    Register("get_device_detail",
             std::make_unique<GetDeviceDetailStrategy>());
    Register("get_device_data",
             std::make_unique<GetDeviceDataStrategy>());
    Register("get_rules",
             std::make_unique<GetRulesStrategy>());
    Register("get_rule_detail",
             std::make_unique<GetRuleDetailStrategy>());
    Register("get_user_profiles",
             std::make_unique<GetUserProfilesStrategy>());
}

SqlStrategy *CmdRouter::Lookup(const std::string &cmd) const
{
    auto it = strategies_.find(cmd);
    if (it == strategies_.end()) {
        return nullptr;
    }
    return it->second.get();
}

void CmdRouter::Register(const std::string &cmd,
                         std::unique_ptr<SqlStrategy> strategy)
{
    strategies_[cmd] = std::move(strategy);
}

// ============================================================================
// AppSqlProxy — Construction / Destruction
// ============================================================================

AppSqlProxy::AppSqlProxy(MqttClient *client)
    : mqtt_client_(client)
{
    using namespace std::placeholders;

    sql_trans_sub_ = std::make_unique<MqttSubscription>(
        "app/sql/trans", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnSqlRequest(topic, payload);
        });
}

AppSqlProxy::~AppSqlProxy()
{
    Stop();
}

// ============================================================================
// AppSqlProxy — Lifecycle
// ============================================================================

bool AppSqlProxy::Start()
{
    if (!mqtt_client_->Subscribe(sql_trans_sub_.get())) {
        spdlog::error("AppSqlProxy: failed to subscribe to app/sql/trans");
        return false;
    }

    spdlog::info("AppSqlProxy: started (subscribed to app/sql/trans)");
    return true;
}

void AppSqlProxy::Stop()
{
    if (mqtt_client_ && sql_trans_sub_) {
        mqtt_client_->Unsubscribe(sql_trans_sub_.get());
    }
    spdlog::info("AppSqlProxy: stopped");
}

// ============================================================================
// AppSqlProxy — MQTT Callback
// ============================================================================

void AppSqlProxy::OnSqlRequest(const std::string & /*topic*/,
                                const std::string &payload)
{
    // 1. Parse JSON
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::warn("AppSqlProxy: failed to parse request JSON: {}", e.what());
        SendResponse("", kRespInvalidJson, nlohmann::json::array());
        return;
    }

    // 2. Validate required fields
    if (!request.contains("msg_id") || !request["msg_id"].is_string() ||
        !request.contains("cmd") || !request["cmd"].is_string()) {
        spdlog::warn("AppSqlProxy: request missing required fields "
                     "(msg_id or cmd)");
        std::string msg_id = request.value("msg_id", "");
        SendResponse(msg_id, kRespMissingField, nlohmann::json::array());
        return;
    }

    // 3. Look up strategy by cmd
    std::string cmd = request["cmd"].get<std::string>();
    SqlStrategy *strategy = router_.Lookup(cmd);
    if (!strategy) {
        spdlog::warn("AppSqlProxy: unknown cmd '{}'", cmd);
        SendResponse(request["msg_id"], kRespUnknownCmd,
                     nlohmann::json::array());
        return;
    }

    // 4. Validate params
    nlohmann::json params = request.value("params", nlohmann::json::object());
    if (!strategy->ValidateParams(params)) {
        spdlog::warn("AppSqlProxy: invalid params for cmd '{}'", cmd);
        SendResponse(request["msg_id"], kRespInvalidParams,
                     nlohmann::json::array());
        return;
    }

    // 5. Execute query
    HandleQuery(request);
}

// ============================================================================
// AppSqlProxy — HandleQuery
// ============================================================================

void AppSqlProxy::HandleQuery(const nlohmann::json &request)
{
    std::string msg_id = request["msg_id"].get<std::string>();
    std::string cmd = request["cmd"].get<std::string>();
    nlohmann::json params = request.value("params", nlohmann::json::object());

    SqlStrategy *strategy = router_.Lookup(cmd);

    // 1. Build SQL from strategy
    std::string sql = strategy->GetSql(params);

    spdlog::debug("AppSqlProxy: executing SQL cmd='{}': {}", cmd, sql);

    // 2. Execute via AppSqlTable::ExecuteRead (inherited from DBTable)
    std::vector<nlohmann::json> rows;
    bool ok = db_.ExecuteRead(
        sql,
        [&](sqlite3_stmt *stmt) -> int {
            if (!strategy->BindParams(stmt, params)) {
                return SQLITE_ERROR;
            }
            return SQLITE_OK;
        },
        [&](sqlite3_stmt *stmt) {
            if (rows.size() < 10000) {
                rows.push_back(RowToJson(stmt));
            }
        });

    if (!ok) {
        spdlog::error("AppSqlProxy: query execution failed for cmd '{}'", cmd);
        SendResponse(msg_id, kRespSqlError, nlohmann::json::array());
        return;
    }

    spdlog::info("AppSqlProxy: cmd '{}' returned {} rows (msg_id={})",
                 cmd, rows.size(), msg_id);
    SendResponse(msg_id, kRespOk, rows);
}

// ============================================================================
// AppSqlProxy — SendResponse
// ============================================================================

void AppSqlProxy::SendResponse(const std::string &msg_id, int resp_code,
                                const nlohmann::json &rows)
{
    nlohmann::json response;
    response["msg_id"] = msg_id;
    response["resp"] = resp_code;
    response["rows"] = rows;
    response["timestamp"] = CurrentTimestamp();

    mqtt_client_->PublishMessage("app/sql/resp", response.dump(), 1);
}

// ============================================================================
// AppSqlProxy — Helpers
// ============================================================================

std::string AppSqlProxy::CurrentTimestamp()
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

nlohmann::json AppSqlProxy::RowToJson(sqlite3_stmt *stmt)
{
    int col_count = sqlite3_column_count(stmt);
    nlohmann::json obj;

    for (int i = 0; i < col_count; ++i) {
        const char *name = sqlite3_column_name(stmt, i);
        std::string col_name = name ? name : "";

        int type = sqlite3_column_type(stmt, i);
        switch (type) {
        case SQLITE_INTEGER:
            obj[col_name] = sqlite3_column_int64(stmt, i);
            break;

        case SQLITE_FLOAT:
            obj[col_name] = sqlite3_column_double(stmt, i);
            break;

        case SQLITE_TEXT: {
            const char *text = reinterpret_cast<const char *>(
                sqlite3_column_text(stmt, i));
            int len = sqlite3_column_bytes(stmt, i);
            obj[col_name] = text ? std::string(text, static_cast<size_t>(len))
                                 : "";
            break;
        }

        case SQLITE_BLOB: {
            const void *blob = sqlite3_column_blob(stmt, i);
            int len = sqlite3_column_bytes(stmt, i);
            if (blob && len == 16) {
                // 16-byte BLOB → UUID string (e.g. dev_id, evt_id, user_id)
                obj[col_name] = util::BlobToUuid(
                    static_cast<const uint8_t *>(blob));
            } else if (blob && len > 0) {
                // Other BLOB → hex string
                static const char hex[] = "0123456789abcdef";
                std::string hex_str;
                hex_str.reserve(static_cast<size_t>(len) * 2);
                const auto *bytes = static_cast<const uint8_t *>(blob);
                for (int j = 0; j < len; ++j) {
                    hex_str += hex[(bytes[j] >> 4) & 0xF];
                    hex_str += hex[bytes[j] & 0xF];
                }
                obj[col_name] = std::move(hex_str);
            } else {
                obj[col_name] = "";
            }
            break;
        }

        case SQLITE_NULL:
        default:
            obj[col_name] = nullptr;
            break;
        }
    }

    return obj;
}

}  // namespace cortexlink
