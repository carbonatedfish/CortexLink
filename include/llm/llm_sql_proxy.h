#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <httplib/httplib.h>

#include "db/db_table.h"
#include "llm/llm_sql_strategy.h"
#include "util/sql_util.h"
#include "util/time_util.h"

namespace cortexlink {

// LlmSqlProxy listens on an HTTP port for LLM (OpenClaw) SQL requests.
// It uses the strategy pattern (LlmSqlStrategy + LlmCmdRouter) to dispatch
// commands to pre-defined SQL templates with parameter binding.
//
// Supports both read operations (device_property, event, rule, event_record)
// and write operations (rule insert/update/delete/enable).
//
// Protocol: HTTP POST /sql with JSON body {"cmd":"...", "params":{...}}.
// Response is JSON: {"resp":0, "rows":[...], "message":"ok", "timestamp":"..."}.
class LlmSqlProxy {
public:
    LlmSqlProxy();
    ~LlmSqlProxy();

    LlmSqlProxy(const LlmSqlProxy &) = delete;
    LlmSqlProxy &operator=(const LlmSqlProxy &) = delete;

    // Set the HTTP listen port before Start().
    // Default: 8899
    void SetPort(int port);

    // Create HTTP server and start the listen thread.
    // Returns false if server setup fails.
    bool Start();

    // Stop the server and join the listen thread.
    void Stop();

private:
    static constexpr int kRespOk            = 0;
    static constexpr int kRespInvalidJson   = 1;
    static constexpr int kRespMissingField  = 2;
    static constexpr int kRespUnknownCmd    = 3;
    static constexpr int kRespInvalidParams = 4;
    static constexpr int kRespSqlError      = 5;

    // HTTP POST /sql handler.
    void HandleRequest(const httplib::Request &req, httplib::Response &res);

    // Core dispatch: validate cmd, look up strategy, execute SQL.
    // Returns the full JSON response envelope.
    nlohmann::json ProcessRequest(const nlohmann::json &request);

    // Build the standard JSON response envelope.
    // Extra fields (e.g. rule_id from PostExecute) are merged in.
    nlohmann::json BuildResponse(int resp_code, const nlohmann::json &rows,
                                  const nlohmann::json &extra = nullptr);

    // Write a JSON object as the HTTP response body.
    static void SendJsonResponse(httplib::Response &res,
                                 const nlohmann::json &body);

    int port_ = 8899;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    std::unique_ptr<httplib::Server> server_;
    PublicDBTable db_;
    LlmCmdRouter router_;
};

}  // namespace cortexlink
