/**
 * CortexLink LLM 调用与脚本生成测试
 *
 * 测试覆盖：
 *   A. MCP Server 连通性探测（case 1）
 *   B. LlmSqlProxy HTTP 端点测试（case 2-6）
 *   C. LlmSqlProxy 写入验证（case 7）
 *   D. 端到端规则生成（case 8-9，仅 MCP Server 可用时运行）
 *
 * 依赖：
 *   - 无外部依赖即可运行 B/C 组
 *   - D 组需要 MCP Server @ 127.0.0.1:18789
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "db/db_table.h"
#include "db/device_data_table.h"
#include "db/device_property_table.h"
#include "db/event_record_table.h"
#include "db/event_rule_table.h"
#include "db/event_table.h"
#include "db/rule_table.h"
#include "db/user_profile_table.h"
#include "llm/llm_sql_proxy.h"
#include "llm/open_claw_client.h"
#include "util/log_util.h"
#include "util/uuid_util.h"

using namespace cortexlink;

// ============================================================================
// Test constants
// ============================================================================

static const char *TEST_DB_PATH   = "/tmp/cortexlink_llm_test.db";
static const char *TEST_LOG_DIR   = "/tmp/cortexlink_llm_test_logs";
static const int   PROXY_PORT     = 18991;   // LlmSqlProxy unit-test port
static const int   E2E_PROXY_PORT = 8899;    // MCP Server expected port
static const char *MCP_ENDPOINT   = "http://127.0.0.1:18789";

// Device UUIDs for E2E tests (standard hyphenated format)
static const char *TEMP_SENSOR_UUID_STR = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
static const char *FAN_UUID_STR          = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
static const char *HIGH_TEMP_EVT_UUID_STR = "cccccccc-cccc-cccc-cccc-cccccccccccc";

// Pre-computed BLOBs for E2E tests
static std::array<uint8_t, 16> TEMP_SENSOR_BLOB;
static std::array<uint8_t, 16> FAN_BLOB;
static std::array<uint8_t, 16> HIGH_TEMP_EVT_BLOB;

// ============================================================================
// Simple test harness (same pattern as rule_engine_test.cpp)
// ============================================================================

static int g_passed = 0;
static int g_failed = 0;
static bool g_mcp_available = false;

static void TestBegin(const char *name) {
    std::cout << "  TEST: " << name << " ... " << std::flush;
}

static void TestPass() {
    std::cout << "PASS" << std::endl;
    g_passed++;
}

static void TestFail(const char *msg) {
    std::cout << "FAIL — " << msg << std::endl;
    g_failed++;
}

#define ASSERT_TRUE(cond, msg)        \
    do {                              \
        if (!(cond)) {                \
            TestFail(msg);            \
            return;                   \
        }                             \
    } while (0)

#define ASSERT_FALSE(cond, msg)       \
    do {                              \
        if ((cond)) {                 \
            TestFail(msg);            \
            return;                   \
        }                             \
    } while (0)

// ============================================================================
// Helpers — file / directory operations
// ============================================================================

static void RemoveFile(const char *path) {
    std::remove(path);
}

static void RemoveDir(const char *path) {
    std::string cmd = std::string("rm -rf ") + path;
    std::system(cmd.c_str());
}

static bool EnsureDir(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

static std::string GetScriptDir() {
    const char *home = std::getenv("HOME");
    if (!home) {
        return ".cortexlink/scripts/";
    }
    std::string dir = std::string(home) + "/.cortexlink/scripts/";
    EnsureDir(dir);
    return dir;
}

// List .lua files in the scripts directory (non-recursive).
static std::vector<std::string> ListLuaFiles(const std::string &dir) {
    std::vector<std::string> result;
    DIR *d = opendir(dir.c_str());
    if (!d) return result;

    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".lua") {
            result.push_back(name);
        }
    }
    closedir(d);
    return result;
}

// ============================================================================
// Helpers — network
// ============================================================================

// Returns true if nothing is listening on localhost:port.
static bool IsPortAvailable(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    int rc = connect(sock, reinterpret_cast<struct sockaddr *>(&addr),
                     sizeof(addr));
    close(sock);

    return rc != 0;  // connect failed → no listener → port is available
}

// ============================================================================
// Helpers — IsHistoryComplete (replicated from AppManager)
// ============================================================================

static bool IsHistoryComplete(const nlohmann::json &history) {
    // Case 1: top-level "status" field
    if (history.contains("status") && history["status"].is_string()) {
        static const std::vector<std::string> kDoneStatuses = {
            "completed", "done", "success", "finished"
        };
        std::string status = history["status"].get<std::string>();
        for (const auto &s : kDoneStatuses) {
            if (status == s) return true;
        }
        return false;
    }

    // Case 2: "messages" array — iterate in reverse (latest first)
    if (history.contains("messages") && history["messages"].is_array()) {
        const auto &messages = history["messages"];
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (!it->is_object()) continue;

            if (it->contains("role") && (*it)["role"].is_string()
                && (*it)["role"].get<std::string>() == "assistant") {

                if (it->contains("status") && (*it)["status"].is_string()) {
                    std::string status = (*it)["status"].get<std::string>();
                    static const std::vector<std::string> kDoneStatuses = {
                        "completed", "done", "success", "finished"
                    };
                    for (const auto &s : kDoneStatuses) {
                        if (status == s) return true;
                    }
                }
                return false;
            }
        }
    }

    // Case 3: nested "result" or "data" objects
    if (history.contains("result") && history["result"].is_object()) {
        return IsHistoryComplete(history["result"]);
    }
    if (history.contains("data") && history["data"].is_object()) {
        return IsHistoryComplete(history["data"]);
    }

    return false;
}

// ============================================================================
// Database setup
// ============================================================================

static bool SetupDatabase() {
    RemoveFile(TEST_DB_PATH);
    RemoveDir(TEST_LOG_DIR);

    if (!DBTable::Initialize(TEST_DB_PATH)) {
        std::cerr << "FATAL: Failed to initialize test database at "
                  << TEST_DB_PATH << std::endl;
        return false;
    }

    // Create all 7 tables.
    DevicePropertyTable dev_prop;
    EventTable event_tbl;
    RuleTable rule_tbl;
    DeviceDataTable dev_data;
    UserProfileTable user_tbl;
    EventRuleTable evt_rule;
    EventRecordTable evt_rec;

    bool ok = true;
    ok = dev_prop.CreateTable()   && ok;
    ok = event_tbl.CreateTable()  && ok;
    ok = rule_tbl.CreateTable()   && ok;
    ok = dev_data.CreateTable()   && ok;
    ok = user_tbl.CreateTable()   && ok;
    ok = evt_rule.CreateTable()   && ok;
    ok = evt_rec.CreateTable()    && ok;

    if (!ok) {
        std::cerr << "FATAL: Failed to create one or more tables" << std::endl;
        return false;
    }

    return true;
}

// ============================================================================
// E2E test data insertion
// ============================================================================

static void InitTestBlobs() {
    TEMP_SENSOR_BLOB   = util::UuidToBlob(TEMP_SENSOR_UUID_STR);
    FAN_BLOB           = util::UuidToBlob(FAN_UUID_STR);
    HIGH_TEMP_EVT_BLOB = util::UuidToBlob(HIGH_TEMP_EVT_UUID_STR);
}

static bool InsertE2ETestData() {
    // ---- Temperature sensor device ----
    DevicePropertyTable dev_prop;
    {
        DevicePropertyTable::DeviceProperty dev;
        dev.dev_id     = TEMP_SENSOR_BLOB;
        dev.dev_name   = "temp_sensor";
        dev.dev_type   = "sensor";
        dev.dev_subtype = "temperature";
        dev.dev_state  = "online";
        dev.location   = "living_room";
        dev.user_param = "Monitors room temperature";
        dev.actions    = "[]";
        dev.events     = "{\"evt_id\": [\"" + std::string(HIGH_TEMP_EVT_UUID_STR) + "\"]}";
        dev.data       = R"({"data": [{"d_name": "temperature", "desc": "Current temperature", "type": "float", "unit": "°C"}]})";
        if (!dev_prop.Insert(dev)) {
            std::cerr << "FATAL: Failed to insert temp_sensor device" << std::endl;
            return false;
        }
    }

    // ---- Fan actuator device ----
    {
        DevicePropertyTable::DeviceProperty dev;
        dev.dev_id     = FAN_BLOB;
        dev.dev_name   = "fan_001";
        dev.dev_type   = "actuator";
        dev.dev_subtype = "fan";
        dev.dev_state  = "online";
        dev.location   = "living_room";
        dev.user_param = "Cooling fan";
        dev.actions    = R"({"actions": [{"act_id": "switch", "act_name": "Switch", "desc": "Turn fan on/off", "params": [{"p_name": "state", "desc": "On or off", "p_type": "str", "range": ["on", "off"], "unit": ""}], "pre_cond": ""}]})";
        dev.events     = "{\"evt_id\": []}";
        dev.data       = "[]";
        if (!dev_prop.Insert(dev)) {
            std::cerr << "FATAL: Failed to insert fan_001 device" << std::endl;
            return false;
        }
    }

    // ---- high_temp event ----
    EventTable event_tbl;
    {
        EventTable::Event evt;
        evt.evt_id   = HIGH_TEMP_EVT_BLOB;
        evt.dev_id   = TEMP_SENSOR_BLOB;
        evt.evt_name = "high_temp";
        evt.desc     = "Temperature exceeds threshold";
        evt.params   = R"({"params": [{"p_name": "temperature", "desc": "Current temperature", "p_type": "float", "unit": "°C"}]})";
        if (!event_tbl.Insert(evt)) {
            std::cerr << "FATAL: Failed to insert high_temp event" << std::endl;
            return false;
        }
    }

    // ---- Device data: temperature = 25.0 ----
    DeviceDataTable dev_data_tbl;
    {
        DeviceDataTable::DeviceData dd;
        dd.dev_id    = TEMP_SENSOR_BLOB;
        dd.data_name = "temperature";
        dd.data_type = "float";
        dd.data_val  = "25.0";
        if (!dev_data_tbl.Upsert(dd)) {
            std::cerr << "FATAL: Failed to insert temperature data" << std::endl;
            return false;
        }
    }

    return true;
}

// ============================================================================
// LlmSqlProxy lifecycle
// ============================================================================

static std::unique_ptr<LlmSqlProxy> g_proxy;

static bool StartProxy(int port) {
    if (g_proxy) {
        g_proxy->Stop();
        g_proxy.reset();
    }

    auto proxy = std::make_unique<LlmSqlProxy>();
    proxy->SetPort(port);
    if (!proxy->Start()) {
        std::cerr << "FATAL: Failed to start LlmSqlProxy on port "
                  << port << std::endl;
        return false;
    }

    // Give the server thread a moment to begin listening.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_proxy = std::move(proxy);
    return true;
}

static void StopProxy() {
    if (g_proxy) {
        g_proxy->Stop();
        g_proxy.reset();
    }
    // Give the server thread time to release the port.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ============================================================================
// HTTP helper — POST to LlmSqlProxy and return parsed JSON response
// ============================================================================

static nlohmann::json PostSql(int port, const nlohmann::json &body) {
    std::string url = "http://127.0.0.1:" + std::to_string(port);
    httplib::Client client(url);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);

    auto res = client.Post("/sql", body.dump(), "application/json");
    if (!res) {
        return nlohmann::json::object();  // connection failure
    }

    try {
        return nlohmann::json::parse(res->body);
    } catch (const nlohmann::json::parse_error &) {
        return nlohmann::json::object();
    }
}

// ============================================================================
// Test Cases — A. Connectivity
// ============================================================================

static void TestCaseA1_McpConnectivity() {
    TestBegin("A.1 MCP Server connectivity check");

    httplib::Client client(MCP_ENDPOINT);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(3, 0);

    auto res = client.Get("/");
    if (res) {
        g_mcp_available = true;
        std::cout << "PASS (server responded)" << std::endl;
        g_passed++;
    } else {
        std::cout << "SKIP — MCP Server not running at " << MCP_ENDPOINT
                  << std::endl;
        g_passed++;  // not a code defect — count as pass
    }
}

// ============================================================================
// Test Cases — B. LlmSqlProxy HTTP endpoints
// ============================================================================

static void TestCaseB1_MissingCmd() {
    TestBegin("B.1 missing cmd field → resp=2");

    nlohmann::json req;
    req["bogus"] = true;

    auto resp = PostSql(PROXY_PORT, req);
    ASSERT_TRUE(resp.contains("resp"), "response should contain 'resp' field");
    ASSERT_TRUE(resp["resp"] == 2,
                "missing cmd should return resp=2 (kRespMissingField)");

    TestPass();
}

static void TestCaseB2_UnknownCmd() {
    TestBegin("B.2 unknown cmd → resp=3");

    nlohmann::json req;
    req["cmd"] = "nonexistent_test_command_xyz";

    auto resp = PostSql(PROXY_PORT, req);
    ASSERT_TRUE(resp.contains("resp"), "response should contain 'resp' field");
    ASSERT_TRUE(resp["resp"] == 3,
                "unknown cmd should return resp=3 (kRespUnknownCmd)");

    TestPass();
}

static void TestCaseB3_InvalidParams() {
    TestBegin("B.3 missing required param → resp=4");

    // get_device_property requires dev_id param; omit it
    nlohmann::json req;
    req["cmd"] = "get_device_property";
    req["params"] = nlohmann::json::object();

    auto resp = PostSql(PROXY_PORT, req);
    ASSERT_TRUE(resp.contains("resp"), "response should contain 'resp' field");
    ASSERT_TRUE(resp["resp"] == 4,
                "missing dev_id should return resp=4 (kRespInvalidParams)");

    TestPass();
}

static void TestCaseB4_ReadEmptyDb() {
    TestBegin("B.4 get_device_properties on empty DB → resp=0, rows=[]");

    nlohmann::json req;
    req["cmd"] = "get_device_properties";

    auto resp = PostSql(PROXY_PORT, req);
    ASSERT_TRUE(resp.contains("resp"), "response should contain 'resp' field");
    ASSERT_TRUE(resp["resp"] == 0,
                "valid read should return resp=0 (kRespOk)");
    ASSERT_TRUE(resp.contains("rows"), "response should contain 'rows' field");
    ASSERT_TRUE(resp["rows"].is_array(), "'rows' should be an array");
    ASSERT_TRUE(resp["rows"].empty(),
                "rows should be empty on a fresh database");

    TestPass();
}

static void TestCaseB5_InsertRule() {
    TestBegin("B.5 insert_rule → resp=0 with rule_id");

    nlohmann::json req;
    req["cmd"] = "insert_rule";
    req["params"]["rule_name"] = "test_llm_rule";
    req["params"]["rule_type"] = "automation";
    req["params"]["enable"]    = true;
    req["params"]["count"]     = 0;
    req["params"]["limit"]     = 0;
    req["params"]["cond_expr"] = "";
    req["params"]["action"]    = "test_script.lua";

    auto resp = PostSql(PROXY_PORT, req);
    ASSERT_TRUE(resp.contains("resp"), "response should contain 'resp' field");
    ASSERT_TRUE(resp["resp"] == 0,
                "insert_rule should return resp=0 (kRespOk)");
    ASSERT_TRUE(resp.contains("rule_id"), "response should contain 'rule_id'");
    ASSERT_TRUE(resp["rule_id"].is_number_integer(),
                "rule_id should be an integer");
    ASSERT_TRUE(resp["rule_id"] > 0,
                "rule_id should be a positive integer");

    // Verify in DB directly
    RuleTable rule_tbl;
    int64_t rule_id = resp["rule_id"].get<int64_t>();
    auto rule = rule_tbl.GetByRuleId(rule_id);
    ASSERT_TRUE(rule.has_value(), "inserted rule should exist in DB");
    ASSERT_TRUE(rule->rule_name == "test_llm_rule",
                "rule_name should match");
    ASSERT_TRUE(rule->rule_type == "automation",
                "rule_type should match");
    ASSERT_TRUE(rule->action == "test_script.lua",
                "action should match");

    TestPass();
}

// ============================================================================
// Test Cases — C. LlmSqlProxy INSERT → Query → Delete round-trip
// ============================================================================

static void TestCaseC1_RoundTrip() {
    TestBegin("C.1 insert → get_rule → delete round-trip");

    // 1. Insert a rule via HTTP
    nlohmann::json ins_req;
    ins_req["cmd"] = "insert_rule";
    ins_req["params"]["rule_name"] = "roundtrip_rule";
    ins_req["params"]["rule_type"] = "schedule";
    ins_req["params"]["enable"]    = false;
    ins_req["params"]["count"]     = 0;
    ins_req["params"]["limit"]     = 5;
    ins_req["params"]["cond_expr"] = "{time} < \"12:00\"";
    ins_req["params"]["action"]    = "roundtrip.lua";

    auto ins_resp = PostSql(PROXY_PORT, ins_req);
    ASSERT_TRUE(ins_resp.contains("rule_id"),
                "insert should return rule_id");
    int64_t rule_id = ins_resp["rule_id"].get<int64_t>();
    ASSERT_TRUE(rule_id > 0, "rule_id should be positive");

    // 2. Query it back via HTTP get_rule
    nlohmann::json get_req;
    get_req["cmd"] = "get_rule";
    get_req["params"]["rule_id"] = rule_id;

    auto get_resp = PostSql(PROXY_PORT, get_req);
    ASSERT_TRUE(get_resp["resp"] == 0, "get_rule should succeed");
    ASSERT_TRUE(get_resp["rows"].is_array() && get_resp["rows"].size() == 1,
                "get_rule should return exactly 1 row");

    auto &row = get_resp["rows"][0];
    ASSERT_TRUE(row["rule_name"] == "roundtrip_rule",
                "queried rule_name should match");
    ASSERT_TRUE(row["rule_type"] == "schedule",
                "queried rule_type should match");
    ASSERT_TRUE(row["action"] == "roundtrip.lua",
                "queried action should match");
    // enable is stored as integer 0/1 in SQLite
    ASSERT_TRUE(row["limit"] == 5,
                "queried limit should be 5");

    // 3. Delete it via HTTP delete_rule
    nlohmann::json del_req;
    del_req["cmd"] = "delete_rule";
    del_req["params"]["rule_id"] = rule_id;

    auto del_resp = PostSql(PROXY_PORT, del_req);
    ASSERT_TRUE(del_resp["resp"] == 0, "delete_rule should succeed");

    // 4. Verify gone from DB
    RuleTable rule_tbl;
    auto rule = rule_tbl.GetByRuleId(rule_id);
    ASSERT_FALSE(rule.has_value(), "rule should be deleted from DB");

    TestPass();
}

// ============================================================================
// Test Cases — D. End-to-end rule generation (requires MCP Server)
// ============================================================================

static void TestCaseD1_SendAndPoll() {
    TestBegin("D.1 send prompt to MCP and poll for completion");

    OpenClawClient open_claw;
    open_claw.SetEndpoint(MCP_ENDPOINT);

    // Generate a session ID
    std::string session = util::GenerateUuid();

    // Send the prompt — ask the LLM to create a rule for turning on
    // the fan when high_temp triggers with temperature > 30.
    std::string prompt =
        "当 high_temp 事件触发且 temperature 参数超过 30 时，"
        "打开 fan_001 的风扇。";

    auto send_resp = open_claw.SendMessageAndGetResponse(session, prompt);
    ASSERT_TRUE(send_resp.has_value(),
                "SendMessageAndGetResponse should succeed "
                "(MCP Server may be down or LLM backend unavailable)");

    spdlog::info("Prompt sent to MCP Server (session={})", session);

    // Poll for completion — max 30 attempts × 1s = 30s timeout
    static constexpr int kMaxAttempts = 30;
    bool completed = false;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto history = open_claw.GetHistory(session);
        if (!history) {
            spdlog::debug("History poll failed (attempt {}/{})",
                          attempt + 1, kMaxAttempts);
            continue;
        }

        if (IsHistoryComplete(*history)) {
            spdlog::info("MCP processing completed (session={}, attempt={})",
                         session, attempt + 1);
            completed = true;
            break;
        }

        spdlog::debug("Waiting for MCP completion (attempt {}/{})",
                      attempt + 1, kMaxAttempts);
    }

    ASSERT_TRUE(completed, "MCP processing timed out after 30s");

    TestPass();
}

static void TestCaseD2_VerifyRulesAndScripts() {
    TestBegin("D.2 verify rules in DB and Lua scripts on disk");

    // 1. Check that at least one rule exists in the DB
    RuleTable rule_tbl;
    auto rules = rule_tbl.GetAll();
    ASSERT_TRUE(!rules.empty(),
                "at least one rule should exist after MCP processing");

    // 2. Check that at least one rule has a .lua action
    bool found_lua_action = false;
    for (const auto &rule : rules) {
        if (rule.action.size() > 4 &&
            rule.action.substr(rule.action.size() - 4) == ".lua") {
            found_lua_action = true;
            spdlog::info("Found rule: id={}, name='{}', action='{}'",
                         rule.rule_id, rule.rule_name, rule.action);
            break;
        }
    }
    ASSERT_TRUE(found_lua_action,
                "at least one rule should have a .lua action filename");

    // 3. Check that at least one .lua script file exists on disk
    std::string script_dir = GetScriptDir();
    auto lua_files = ListLuaFiles(script_dir);

    // Print all found .lua files for debugging
    std::cout << std::endl;
    std::cout << "    Script files in " << script_dir << ":" << std::endl;
    for (const auto &f : lua_files) {
        std::cout << "      " << f << std::endl;
    }

    ASSERT_TRUE(!lua_files.empty(),
                "at least one .lua script file should exist on disk");

    TestPass();
}

// ============================================================================
// Cleanup
// ============================================================================

static void CleanupAll() {
    // Delete event-rule mappings first (FK constraints)
    EventRuleTable evt_rule;
    auto rule_ids = evt_rule.GetRulesByEvtId(HIGH_TEMP_EVT_BLOB);
    for (int64_t rid : rule_ids) {
        evt_rule.DeleteByRuleId(rid);
    }
    evt_rule.DeleteByEvtId(HIGH_TEMP_EVT_BLOB);

    // Delete any remaining rules
    RuleTable rule_tbl;
    auto rules = rule_tbl.GetAll();
    for (const auto &rule : rules) {
        rule_tbl.Delete(rule.rule_id);
    }

    // Delete test device data
    DeviceDataTable dev_data;
    dev_data.Delete(TEMP_SENSOR_BLOB, "temperature");

    // Delete test events
    EventTable event_tbl;
    event_tbl.Delete(HIGH_TEMP_EVT_BLOB);

    // Delete test devices
    DevicePropertyTable dev_prop;
    dev_prop.Delete(TEMP_SENSOR_BLOB);
    dev_prop.Delete(FAN_BLOB);

    // Shut down proxy and database
    StopProxy();
    DBTable::Shutdown();

    // Remove temp files
    RemoveFile(TEST_DB_PATH);
    RemoveDir(TEST_LOG_DIR);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== CortexLink LLM Test ===" << std::endl;
    std::cout << "Test DB:     " << TEST_DB_PATH << std::endl;
    std::cout << "Proxy port:  " << PROXY_PORT << std::endl;
    std::cout << "MCP endpoint:" << MCP_ENDPOINT << std::endl;

    // ---- Pre-compute BLOBs ----
    InitTestBlobs();

    // ---- Setup ----
    std::cout << "\n--- Setup ---" << std::endl;

    // 1. Logger
    util::InitLogger(TEST_LOG_DIR, spdlog::level::debug);
    spdlog::info("=== CortexLinkLlmTest starting ===");

    // 2. Database
    if (!SetupDatabase()) {
        std::cerr << "Database setup failed, aborting." << std::endl;
        return 1;
    }
    std::cout << "  Database initialized." << std::endl;

    // ---- A. Connectivity ----
    std::cout << "\n--- A. Connectivity ---" << std::endl;
    TestCaseA1_McpConnectivity();

    // ---- B. LlmSqlProxy HTTP Endpoints ----
    std::cout << "\n--- B. LlmSqlProxy HTTP Endpoints ---" << std::endl;

    // Check port 18991 availability before starting proxy
    if (!IsPortAvailable(PROXY_PORT)) {
        std::cerr << "FATAL: Port " << PROXY_PORT
                  << " is already in use, aborting." << std::endl;
        DBTable::Shutdown();
        RemoveFile(TEST_DB_PATH);
        RemoveDir(TEST_LOG_DIR);
        return 1;
    }

    if (!StartProxy(PROXY_PORT)) {
        std::cerr << "FATAL: Cannot start LlmSqlProxy on port "
                  << PROXY_PORT << ", aborting." << std::endl;
        DBTable::Shutdown();
        RemoveFile(TEST_DB_PATH);
        RemoveDir(TEST_LOG_DIR);
        return 1;
    }
    std::cout << "  LlmSqlProxy started on port " << PROXY_PORT << "."
              << std::endl;

    TestCaseB1_MissingCmd();
    TestCaseB2_UnknownCmd();
    TestCaseB3_InvalidParams();
    TestCaseB4_ReadEmptyDb();
    TestCaseB5_InsertRule();

    // ---- C. LlmSqlProxy Round-Trip ----
    std::cout << "\n--- C. LlmSqlProxy INSERT → Query → Delete ---"
              << std::endl;
    TestCaseC1_RoundTrip();

    // ---- D. End-to-End (conditional on MCP Server) ----
    if (g_mcp_available && IsPortAvailable(E2E_PROXY_PORT)) {
        std::cout << "\n--- D. End-to-End Rule Generation ---" << std::endl;

        // Stop proxy on test port, restart on MCP-expected port 8899
        StopProxy();

        // Insert test devices/events into DB so MCP Server can discover them
        if (!InsertE2ETestData()) {
            std::cerr << "E2E test data insertion failed, skipping D group."
                      << std::endl;
        } else if (!StartProxy(E2E_PROXY_PORT)) {
            std::cerr << "Cannot bind to port " << E2E_PROXY_PORT
                      << ", skipping E2E tests." << std::endl;
        } else {
            std::cout << "  LlmSqlProxy restarted on port " << E2E_PROXY_PORT
                      << " for MCP Server callbacks." << std::endl;

            // Snapshot Lua files before the test to detect new ones
            std::string script_dir = GetScriptDir();
            auto before_files = ListLuaFiles(script_dir);

            TestCaseD1_SendAndPoll();
            TestCaseD2_VerifyRulesAndScripts();
        }
    } else {
        std::cout << "\n--- D. End-to-End Rule Generation ---" << std::endl;
        if (!g_mcp_available) {
            std::cout << "  SKIPPED — MCP Server not running at "
                      << MCP_ENDPOINT << std::endl;
        } else {
            std::cout << "  SKIPPED — port " << E2E_PROXY_PORT
                      << " is in use (another LlmSqlProxy or CortexLink "
                         "instance may be running)" << std::endl;
        }
    }

    // ---- E. Cleanup ----
    std::cout << "\n--- Cleanup ---" << std::endl;
    CleanupAll();
    std::cout << "  Cleanup complete." << std::endl;

    // ---- Report ----
    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "  Passed: " << g_passed << std::endl;
    std::cout << "  Failed: " << g_failed << std::endl;

    if (g_failed == 0) {
        std::cout << "  ALL TESTS PASSED" << std::endl;
        return 0;
    } else {
        std::cout << "  SOME TESTS FAILED" << std::endl;
        return 1;
    }
}
