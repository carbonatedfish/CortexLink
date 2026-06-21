#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "app/sql_strategy.h"
#include "db/db_table.h"
#include "mqtt/mqtt_client.h"

namespace cortexlink {

// Thin subclass of DBTable that exposes ExecuteRead publicly.
// AppSqlProxy uses this to execute strategy-generated SQL queries
// through the same mutex-serialized connection all other DB code shares.
class AppSqlTable : public DBTable {
public:
    // No real table schema — the 7 tables are already created in main.cpp.
    bool CreateTable() override { return true; }

    // Expose the protected ExecuteRead overloads as public.
    using DBTable::ExecuteRead;
};

// Maps cmd strings to SqlStrategy instances.
class CmdRouter {
public:
    CmdRouter();

    // Look up a strategy by cmd. Returns nullptr if cmd is not registered.
    SqlStrategy *Lookup(const std::string &cmd) const;

private:
    void Register(const std::string &cmd,
                  std::unique_ptr<SqlStrategy> strategy);

    std::unordered_map<std::string, std::unique_ptr<SqlStrategy>> strategies_;
};

// AppSqlProxy receives cmd-driven SQL query requests from the APP,
// dispatches them through the strategy pattern, and responds with results.
class AppSqlProxy {
public:
    explicit AppSqlProxy(MqttClient *client);
    ~AppSqlProxy();

    AppSqlProxy(const AppSqlProxy &) = delete;
    AppSqlProxy &operator=(const AppSqlProxy &) = delete;

    bool Start();
    void Stop();

private:
    static constexpr int kRespOk            = 0;
    static constexpr int kRespInvalidJson   = 1;
    static constexpr int kRespMissingField  = 2;
    static constexpr int kRespUnknownCmd    = 3;
    static constexpr int kRespInvalidParams = 4;
    static constexpr int kRespSqlError      = 5;

    void OnSqlRequest(const std::string &topic, const std::string &payload);
    void HandleQuery(const nlohmann::json &request);
    void SendResponse(const std::string &msg_id, int resp_code,
                      const nlohmann::json &rows);

    static std::string CurrentTimestamp();
    static nlohmann::json RowToJson(sqlite3_stmt *stmt);

    MqttClient *mqtt_client_;
    std::unique_ptr<MqttSubscription> sql_trans_sub_;
    AppSqlTable db_;
    CmdRouter router_;
};

}  // namespace cortexlink
