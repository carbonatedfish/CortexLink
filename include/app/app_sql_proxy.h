#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "app/sql_strategy.h"
#include "db/db_table.h"
#include "mqtt/mqtt_client.h"
#include "util/sql_util.h"
#include "util/time_util.h"

namespace cortexlink {

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
//
// Architecture (matching LlmSqlProxy):
//   OnSqlRequest (transport) → ProcessRequest (pure logic) → SendResponse
//   ProcessRequest returns a JSON envelope built by BuildResponse.
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

    // Transport layer: MQTT callback.
    void OnSqlRequest(const std::string &topic, const std::string &payload);

    // Core dispatch: validate cmd, look up strategy, execute SQL.
    // Returns the full JSON response envelope (without msg_id).
    nlohmann::json ProcessRequest(const nlohmann::json &request);

    // Build the standard JSON response envelope with message field.
    nlohmann::json BuildResponse(int resp_code,
                                  const nlohmann::json &rows,
                                  const nlohmann::json &extra = nullptr);

    // Transport layer: inject msg_id and publish via MQTT.
    void SendResponse(const std::string &msg_id,
                      const nlohmann::json &response);

    MqttClient *mqtt_client_;
    std::unique_ptr<MqttSubscription> sql_trans_sub_;
    PublicDBTable db_;
    CmdRouter router_;
};

}  // namespace cortexlink
