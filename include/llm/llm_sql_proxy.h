#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "db/db_table.h"
#include "llm/llm_sql_strategy.h"

namespace cortexlink {

// Thin subclass of DBTable that exposes ExecuteRead and ExecuteWrite publicly.
// LlmSqlProxy uses this to execute strategy-generated SQL queries
// through the same mutex-serialized connection all other DB code shares.
class LlmSqlTable : public DBTable {
public:
    // No real table schema — tables are already created in main.cpp.
    bool CreateTable() override { return true; }

    // Expose the protected ExecuteRead and ExecuteWrite overloads as public.
    using DBTable::ExecuteRead;
    using DBTable::ExecuteWrite;
};

// LlmSqlProxy listens on a Unix domain socket for LLM (OpenClaw) SQL requests.
// It uses the strategy pattern (LlmSqlStrategy + LlmCmdRouter) to dispatch
// commands to pre-defined SQL templates with parameter binding.
//
// Supports both read operations (device_property, event, rule, event_record)
// and write operations (rule insert/update/delete/enable).
//
// Connection model: one JSON request per connection (accept → read → process
// → write → close). Newline-delimited JSON protocol.
class LlmSqlProxy {
public:
    LlmSqlProxy();
    ~LlmSqlProxy();

    LlmSqlProxy(const LlmSqlProxy &) = delete;
    LlmSqlProxy &operator=(const LlmSqlProxy &) = delete;

    // Set the Unix socket path before Start().
    // Default: ~/.cortexlink/llm_sql.sock
    void SetSocketPath(const std::string &path);

    // Create socket, bind, listen, start accept thread.
    // Returns false if socket setup fails.
    bool Start();

    // Signal accept thread to stop, close socket, unlink socket file.
    void Stop();

private:
    static constexpr int kRespOk            = 0;
    static constexpr int kRespInvalidJson   = 1;
    static constexpr int kRespMissingField  = 2;
    static constexpr int kRespUnknownCmd    = 3;
    static constexpr int kRespInvalidParams = 4;
    static constexpr int kRespSqlError      = 5;

    // Thread entry point. Creates socket, accepts connections.
    void AcceptLoop();

    // Handle a single client connection (read request, dispatch, respond).
    void HandleClient(int client_fd);

    // Read a newline-delimited JSON request from the socket.
    // On failure, sends an error response and returns false.
    bool ReadRequest(int client_fd, nlohmann::json &request);

    // Dispatch the request via LlmCmdRouter, execute SQL, send response.
    void ProcessAndRespond(int client_fd, const nlohmann::json &request);

    // Serialize and send a JSON response over the socket.
    // Extra fields (e.g. rule_id from PostExecute) are merged in.
    void SendResponse(int client_fd, int resp_code,
                      const nlohmann::json &rows,
                      const nlohmann::json &extra = nullptr);

    // Helpers
    static std::string CurrentTimestamp();
    static nlohmann::json RowToJson(sqlite3_stmt *stmt);
    static std::string SocketPathOrDefault();

    std::string socket_path_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    LlmSqlTable db_;
    LlmCmdRouter router_;
};

}  // namespace cortexlink
