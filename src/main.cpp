#include <atomic>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "app/app_manager.h"
#include "app/app_sql_proxy.h"
#include "db/db_table.h"
#include "llm/llm_sql_proxy.h"
#include "db/device_data_table.h"
#include "db/device_property_table.h"
#include "db/event_record_table.h"
#include "db/event_rule_table.h"
#include "db/event_table.h"
#include "db/rule_table.h"
#include "db/user_profile_table.h"
#include "device/device_manager.h"
#include "mqtt/mqtt_client.h"
#include "rule_engine/rule_engine.h"
#include "util/log_util.h"

using namespace cortexlink;

// Global flag for graceful shutdown on SIGINT / SIGTERM.
static std::atomic<bool> g_shutdown{false};

static void SignalHandler(int signum)
{
    spdlog::info("Signal {} received, initiating graceful shutdown", signum);
    g_shutdown = true;
}

int main(int argc, char **argv)
{
    // 1. Install signal handlers.
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // 2. Resolve paths.
    const char *home = std::getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME environment variable not set\n");
        return 1;
    }
    std::string log_dir = std::string(home) + "/.cortexlink/logs";
    std::string db_path = std::string(home) + "/.cortexlink/cortexlink.db";

    // 3. Initialize logger.
    util::InitLogger(log_dir, spdlog::level::info);
    spdlog::info("=== CortexLink starting ===");

    // 4. Initialize database.
    if (!DBTable::Initialize(db_path)) {
        spdlog::error("Failed to initialize database at {}", db_path);
        return 1;
    }

    // 5. Create all tables.
    DevicePropertyTable dev_prop_table;
    EventTable event_table;
    RuleTable rule_table;
    DeviceDataTable dev_data_table;
    UserProfileTable user_table;
    EventRuleTable evt_rule_table;
    EventRecordTable evt_record_table;

    bool all_ok = true;
    all_ok = dev_prop_table.CreateTable() && all_ok;
    all_ok = event_table.CreateTable() && all_ok;
    all_ok = rule_table.CreateTable() && all_ok;
    all_ok = dev_data_table.CreateTable() && all_ok;
    all_ok = user_table.CreateTable() && all_ok;
    all_ok = evt_rule_table.CreateTable() && all_ok;
    all_ok = evt_record_table.CreateTable() && all_ok;

    if (!all_ok) {
        spdlog::error("Failed to create one or more database tables");
        DBTable::Shutdown();
        return 1;
    }
    spdlog::info("All database tables created/verified");

    // 6. Create MQTT client and connect to the broker.
    MqttClient mqtt("cortexlink-host");
    // Uncomment to set credentials if the broker requires them:
    // mqtt.SetCredentials("username", "password");
    if (!mqtt.Connect()) {
        spdlog::error("Failed to connect to MQTT broker");
        DBTable::Shutdown();
        return 1;
    }

    // 7. Start the MQTT loop on a background thread.
    if (!mqtt.LoopStart()) {
        spdlog::error("Failed to start MQTT loop");
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 8. Create and start the DeviceManager.
    DeviceManager dev_mgr(&mqtt);
    if (!dev_mgr.Start()) {
        spdlog::error("Failed to start DeviceManager");
        mqtt.LoopStop();
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 9. Create and start the RuleEngine.
    RuleEngine rule_engine(&mqtt, &dev_mgr);
    if (!rule_engine.Start()) {
        spdlog::error("Failed to start RuleEngine");
        dev_mgr.Stop();
        mqtt.LoopStop();
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 10. Create and start the AppManager (file transfer).
    AppFileTransManager app_mgr(&mqtt);
    if (!app_mgr.Start()) {
        spdlog::error("Failed to start AppManager");
        rule_engine.Stop();
        dev_mgr.Stop();
        mqtt.LoopStop();
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 11. Create and start the AppSqlProxy.
    AppSqlProxy sql_proxy(&mqtt);
    if (!sql_proxy.Start()) {
        spdlog::error("Failed to start AppSqlProxy");
        app_mgr.Stop();
        rule_engine.Stop();
        dev_mgr.Stop();
        mqtt.LoopStop();
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 12. Create and start the LlmSqlProxy (Unix domain socket for LLM).
    LlmSqlProxy llm_sql_proxy;
    if (!llm_sql_proxy.Start()) {
        spdlog::error("Failed to start LlmSqlProxy");
        sql_proxy.Stop();
        app_mgr.Stop();
        rule_engine.Stop();
        dev_mgr.Stop();
        mqtt.LoopStop();
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    spdlog::info("CortexLink startup complete — waiting for events");

    // 13. Main loop — wait for shutdown signal.
    while (!g_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 14. Graceful shutdown (reverse order of startup).
    spdlog::info("=== CortexLink shutting down ===");

    llm_sql_proxy.Stop();
    sql_proxy.Stop();
    app_mgr.Stop();
    rule_engine.Stop();
    dev_mgr.Stop();
    mqtt.LoopStop();
    mqtt.Disconnect();
    DBTable::Shutdown();

    spdlog::info("=== CortexLink shutdown complete ===");
    return 0;
}
