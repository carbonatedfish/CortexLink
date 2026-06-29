#include "main.h"

using namespace cortexlink;

// Global flag for graceful shutdown on SIGINT / SIGTERM.
static std::atomic<bool> g_shutdown{false};

static void SignalHandler(int signum)
{
    spdlog::info("Signal {} received, initiating graceful shutdown", signum);
    g_shutdown = true;
}

static spdlog::level::level_enum ParseLogLevel(const std::string &level)
{
    if (level == "trace")    return spdlog::level::trace;
    if (level == "debug")    return spdlog::level::debug;
    if (level == "info")     return spdlog::level::info;
    if (level == "warn")     return spdlog::level::warn;
    if (level == "err")      return spdlog::level::err;
    if (level == "critical") return spdlog::level::critical;
    if (level == "off")      return spdlog::level::off;
    return spdlog::level::info;
}

int main(int argc, char **argv)
{
    // 1. Install signal handlers.
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // 2. Parse command-line arguments.
    cxxopts::Options options("CortexLink", "AI + Smart Home Hub");
    options.add_options()
        ("mqtt-host",     "MQTT broker address",            cxxopts::value<std::string>()->default_value("localhost"))
        ("mqtt-port",     "MQTT broker port",               cxxopts::value<int>()->default_value("1883"))
        ("openclaw-endpoint", "OpenClaw/MCP endpoint URL",  cxxopts::value<std::string>()->default_value("http://127.0.0.1:18789"))
        ("llm-sql-port",  "LlmSqlProxy HTTP listen port",   cxxopts::value<int>()->default_value("8899"))
        ("log-level",     "Log level (trace/debug/info/warn/err/critical/off)",
                                                             cxxopts::value<std::string>()->default_value("info"))
        ("h,help",        "Print usage");

    auto cli = options.parse(argc, argv);

    if (cli.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    std::string mqtt_host         = cli["mqtt-host"].as<std::string>();
    int         mqtt_port         = cli["mqtt-port"].as<int>();
    std::string openclaw_endpoint = cli["openclaw-endpoint"].as<std::string>();
    int         llm_sql_port      = cli["llm-sql-port"].as<int>();
    auto        log_level         = ParseLogLevel(cli["log-level"].as<std::string>());

    // 3. Resolve paths.
    const char *home = std::getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME environment variable not set\n");
        return 1;
    }
    std::string log_dir = std::string(home) + "/.cortexlink/logs";
    std::string db_path = std::string(home) + "/.cortexlink/cortexlink.db";

    // 4. Initialize logger.
    util::InitLogger(log_dir, log_level);
    spdlog::info("=== CortexLink starting ===");

    spdlog::debug("CortexLink config: mqtt={}:{} openclaw={} llm_sql_port={} db={} log_level={}",
                  mqtt_host, mqtt_port, openclaw_endpoint, llm_sql_port, db_path,
                  cli["log-level"].as<std::string>());

    // 5. Initialize database.
    if (!DBTable::Initialize(db_path)) {
        spdlog::error("Failed to initialize database at {}", db_path);
        return 1;
    }

    // 6. Create all tables.
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

    // 7. Create MQTT client and connect to the broker.
    MqttClient mqtt("cortexlink-host");
    // Uncomment to set credentials if the broker requires them:
    // mqtt.SetCredentials("username", "password");
    if (!mqtt.Connect(mqtt_host, mqtt_port)) {
        spdlog::error("Failed to connect to MQTT broker");
        DBTable::Shutdown();
        return 1;
    }

    // 8. Start the MQTT loop on a background thread.
    if (!mqtt.LoopStart()) {
        spdlog::error("Failed to start MQTT loop");
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 9. Create and start the DeviceManager.
    DeviceManager dev_mgr(&mqtt);
    if (!dev_mgr.Start()) {
        spdlog::error("Failed to start DeviceManager");
        mqtt.LoopStop();
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 10. Create OpenClawClient for LLM-based rule generation.
    OpenClawClient open_claw_client;
    open_claw_client.SetEndpoint(openclaw_endpoint);

    // 11. Create and start the RuleEngine.
    RuleEngine rule_engine(&mqtt, &dev_mgr, &open_claw_client);
    if (!rule_engine.Start()) {
        spdlog::error("Failed to start RuleEngine");
        dev_mgr.Stop();
        mqtt.LoopStop();
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 12. Create and start the AppManager (file transfer).
    AppManager app_mgr(&mqtt, openclaw_endpoint);
    if (!app_mgr.Start()) {
        spdlog::error("Failed to start AppManager");
        rule_engine.Stop();
        dev_mgr.Stop();
        mqtt.LoopStop();
        mqtt.Disconnect();
        DBTable::Shutdown();
        return 1;
    }

    // 13. Create and start the AppSqlProxy.
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

    // 14. Create and start the LlmSqlProxy (HTTP server).
    LlmSqlProxy llm_sql_proxy;
    llm_sql_proxy.SetPort(llm_sql_port);
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

    // 16. Main loop — wait for shutdown signal.
    while (!g_shutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 17. Graceful shutdown (reverse order of startup).
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
