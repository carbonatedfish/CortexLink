#include "llm/llm_sql_proxy.h"

#include <spdlog/spdlog.h>

namespace cortexlink {

// ============================================================================
// LlmSqlProxy — Construction / Destruction
// ============================================================================

LlmSqlProxy::LlmSqlProxy() = default;

LlmSqlProxy::~LlmSqlProxy()
{
    Stop();
}

// ============================================================================
// LlmSqlProxy — Port Configuration
// ============================================================================

void LlmSqlProxy::SetPort(int port)
{
    port_ = port;
}

// ============================================================================
// LlmSqlProxy — Lifecycle
// ============================================================================

bool LlmSqlProxy::Start()
{
    if (running_) {
        spdlog::warn("LlmSqlProxy: already running");
        return true;
    }

    server_ = std::make_unique<httplib::Server>();

    server_->Post("/sql", [this](const httplib::Request &req,
                                 httplib::Response &res) {
        HandleRequest(req, res);
    });

    running_ = true;
    server_thread_ = std::thread([this]() {
        if (!server_->listen("0.0.0.0", port_)) {
            spdlog::error("LlmSqlProxy: failed to listen on port {}", port_);
        }
        running_ = false;
    });

    // Give the thread a moment to start listening.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!running_) {
        // listen failed and cleared the flag.
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        server_.reset();
        return false;
    }

    spdlog::info("LlmSqlProxy: started (HTTP on port {})", port_);
    return true;
}

void LlmSqlProxy::Stop()
{
    if (!running_) return;

    running_ = false;

    if (server_) {
        server_->stop();
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    server_.reset();

    spdlog::info("LlmSqlProxy: stopped");
}

// ============================================================================
// LlmSqlProxy — HTTP Request Handler
// ============================================================================

void LlmSqlProxy::HandleRequest(const httplib::Request &req,
                                httplib::Response &res)
{
    nlohmann::json request;

    try {
        request = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::warn("LlmSqlProxy: failed to parse request JSON: {}", e.what());
        SendJsonResponse(res, BuildResponse(kRespInvalidJson,
                                            nlohmann::json::array()));
        return;
    }

    spdlog::debug("LlmSqlProxy: request cmd='{}'", request.value("cmd", "?"));

    nlohmann::json response = ProcessRequest(request);
    SendJsonResponse(res, response);
}

// ============================================================================
// LlmSqlProxy — Core Dispatch
// ============================================================================

nlohmann::json LlmSqlProxy::ProcessRequest(const nlohmann::json &request)
{
    // 1. Validate cmd field
    if (!request.contains("cmd") || !request["cmd"].is_string()) {
        spdlog::warn("LlmSqlProxy: request missing required field 'cmd'");
        return BuildResponse(kRespMissingField, nlohmann::json::array());
    }

    std::string cmd = request["cmd"].get<std::string>();

    // 2. Look up strategy
    LlmSqlStrategy *strategy = router_.Lookup(cmd);
    if (!strategy) {
        spdlog::warn("LlmSqlProxy: unknown cmd '{}'", cmd);
        return BuildResponse(kRespUnknownCmd, nlohmann::json::array());
    }

    // 3. Validate params
    nlohmann::json params = request.value("params", nlohmann::json::object());
    if (!strategy->ValidateParams(params)) {
        spdlog::warn("LlmSqlProxy: invalid params for cmd '{}'", cmd);
        return BuildResponse(kRespInvalidParams, nlohmann::json::array());
    }

    // 4. Build SQL
    std::string sql = strategy->GetSql(params);

    spdlog::debug("LlmSqlProxy: executing SQL cmd='{}': {}", cmd, sql);

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
        spdlog::error("LlmSqlProxy: SQL execution failed for cmd '{}'", cmd);
        return BuildResponse(kRespSqlError, nlohmann::json::array());
    }

    spdlog::info("LlmSqlProxy: cmd '{}' returned {} rows",
                 cmd, rows.size());
    spdlog::debug("LlmSqlProxy: response cmd='{}' resp={} rows={}",
                  cmd, kRespOk, rows.size());
    return BuildResponse(kRespOk, rows, extra);
}

// ============================================================================
// LlmSqlProxy — Response Helpers
// ============================================================================

nlohmann::json LlmSqlProxy::BuildResponse(int resp_code,
                                           const nlohmann::json &rows,
                                           const nlohmann::json &extra)
{
    nlohmann::json response;
    response["resp"] = resp_code;
    response["rows"] = rows;
    response["timestamp"] = util::CurrentTimestamp();

    // Human-readable message
    switch (resp_code) {
    case kRespOk:            response["message"] = "ok"; break;
    case kRespInvalidJson:   response["message"] = "Invalid JSON"; break;
    case kRespMissingField:  response["message"] = "Missing 'cmd' field"; break;
    case kRespUnknownCmd:    response["message"] = "Unknown command"; break;
    case kRespInvalidParams: response["message"] = "Invalid parameters"; break;
    case kRespSqlError:      response["message"] = "SQL execution error"; break;
    default:                 response["message"] = "Unknown error"; break;
    }

    // Merge extra fields (e.g. rule_id from insert_rule PostExecute)
    if (!extra.is_null() && extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            response[it.key()] = it.value();
        }
    }

    return response;
}

void LlmSqlProxy::SendJsonResponse(httplib::Response &res,
                                    const nlohmann::json &body)
{
    res.set_content(body.dump(), "application/json");
}

}  // namespace cortexlink
