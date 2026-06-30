#include "app/app_sql_proxy.h"

#include <vector>

#include <spdlog/spdlog.h>

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
    Register("insert_user_profile",
             std::make_unique<InsertUserProfileStrategy>());
    Register("update_user_profile",
             std::make_unique<UpdateUserProfileStrategy>());
    Register("delete_user_profile",
             std::make_unique<DeleteUserProfileStrategy>());
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
// AppSqlProxy — Transport Layer (MQTT)
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
        SendResponse("", BuildResponse(kRespInvalidJson,
                                       nlohmann::json::array()));
        return;
    }

    // 2. Validate msg_id (transport-level concern)
    if (!request.contains("msg_id") || !request["msg_id"].is_string()) {
        spdlog::warn("AppSqlProxy: request missing required field 'msg_id'");
        std::string msg_id = request.value("msg_id", "");
        SendResponse(msg_id, BuildResponse(kRespMissingField,
                                           nlohmann::json::array()));
        return;
    }

    // 3. Delegate to pure logic layer
    nlohmann::json response = ProcessRequest(request);

    // 4. Inject msg_id and publish
    SendResponse(request["msg_id"], response);
}

// ============================================================================
// AppSqlProxy — Core Dispatch (Pure Logic)
// ============================================================================

nlohmann::json AppSqlProxy::ProcessRequest(const nlohmann::json &request)
{
    // 1. Validate cmd field
    if (!request.contains("cmd") || !request["cmd"].is_string()) {
        spdlog::warn("AppSqlProxy: request missing required field 'cmd'");
        return BuildResponse(kRespMissingField, nlohmann::json::array());
    }

    std::string cmd = request["cmd"].get<std::string>();

    // 2. Look up strategy
    SqlStrategy *strategy = router_.Lookup(cmd);
    if (!strategy) {
        spdlog::warn("AppSqlProxy: unknown cmd '{}'", cmd);
        return BuildResponse(kRespUnknownCmd, nlohmann::json::array());
    }

    // 3. Validate params
    nlohmann::json params = request.value("params", nlohmann::json::object());
    if (!strategy->ValidateParams(params)) {
        spdlog::warn("AppSqlProxy: invalid params for cmd '{}'", cmd);
        return BuildResponse(kRespInvalidParams, nlohmann::json::array());
    }

    // 4. Build SQL
    std::string sql = strategy->GetSql(params);

    spdlog::debug("AppSqlProxy: executing SQL cmd='{}': {}", cmd, sql);

    // 5. Execute
    bool ok = false;
    nlohmann::json extra;
    std::vector<nlohmann::json> rows;

    auto bind_fn = [&](sqlite3_stmt *stmt) -> int {
        return strategy->BindParams(stmt, params) ? SQLITE_OK : SQLITE_ERROR;
    };

    if (strategy->IsWrite()) {
        ok = db_.ExecuteWrite(sql, bind_fn);
        if (ok) {
            extra = strategy->PostExecute(DBTable::GetDB());
        }
    } else {
        auto row_fn = [&](sqlite3_stmt *stmt) {
            if (rows.size() < 10000) {
                rows.push_back(util::RowToJson(stmt));
            }
        };
        ok = db_.ExecuteRead(sql, bind_fn, row_fn);
    }

    if (!ok) {
        spdlog::error("AppSqlProxy: SQL execution failed for cmd '{}'", cmd);
        return BuildResponse(kRespSqlError, nlohmann::json::array());
    }

    spdlog::info("AppSqlProxy: cmd '{}' returned {} rows", cmd, rows.size());
    return BuildResponse(kRespOk, rows, extra);
}

// ============================================================================
// AppSqlProxy — Response Helpers
// ============================================================================

nlohmann::json AppSqlProxy::BuildResponse(int resp_code,
                                           const nlohmann::json &rows,
                                           const nlohmann::json &extra)
{
    nlohmann::json response;
    response["resp"] = resp_code;
    response["rows"] = rows;
    response["timestamp"] = util::CurrentTimestamp();

    // Human-readable message (matching LlmSqlProxy format)
    switch (resp_code) {
    case kRespOk:            response["message"] = "ok"; break;
    case kRespInvalidJson:   response["message"] = "Invalid JSON"; break;
    case kRespMissingField:  response["message"] = "Missing required field"; break;
    case kRespUnknownCmd:    response["message"] = "Unknown command"; break;
    case kRespInvalidParams: response["message"] = "Invalid parameters"; break;
    case kRespSqlError:      response["message"] = "SQL execution error"; break;
    default:                 response["message"] = "Unknown error"; break;
    }

    // Merge extra fields (e.g. generated keys from PostExecute)
    if (!extra.is_null() && extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            response[it.key()] = it.value();
        }
    }

    return response;
}

void AppSqlProxy::SendResponse(const std::string &msg_id,
                                const nlohmann::json &response)
{
    nlohmann::json resp = response;
    resp["msg_id"] = msg_id;
    mqtt_client_->PublishMessage("app/sql/resp", resp.dump(), 1);
}

}  // namespace cortexlink
