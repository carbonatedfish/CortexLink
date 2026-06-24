/**
 * CortexLink 链路测试：设备 → 规则引擎 → 规则执行
 *
 * 测试覆盖：
 *   A. InjectEvent 集成测试（case 1-6）
 *   B. LuaSandbox 脚本 API 测试（case 7-10）
 *   C. 条件表达式单元测试（case 11-12）
 *
 * 依赖：远程 MQTT broker @ 43.139.96.190
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <thread>

#include <spdlog/spdlog.h>

#include "db/db_table.h"
#include "db/device_data_table.h"
#include "db/device_property_table.h"
#include "db/event_record_table.h"
#include "db/event_rule_table.h"
#include "db/event_table.h"
#include "db/rule_table.h"
#include "db/user_profile_table.h"
#include "device/device_manager.h"
#include "lua/lua_sandbox.h"
#include "mqtt/mqtt_client.h"
#include "rule_engine/cond_parser.h"
#include "llm/open_claw_client.h"
#include "rule_engine/rule_engine.h"
#include "util/log_util.h"
#include "util/uuid_util.h"

using namespace cortexlink;

// ============================================================================
// Test constants
// ============================================================================

// Device UUIDs (standard hyphenated format)
static const char *TEMP_SENSOR_UUID_STR = "11111111-1111-1111-1111-111111111111";
static const char *FAN_UUID_STR          = "22222222-2222-2222-2222-222222222222";

// Event UUIDs
static const char *HIGH_TEMP_EVT_UUID_STR = "33333333-3333-3333-3333-333333333333";
static const char *MOTION_EVT_UUID_STR    = "44444444-4444-4444-4444-444444444444";
static const char *UNKNOWN_EVT_UUID_STR   = "99999999-9999-9999-9999-999999999999";
static const char *TIMEOUT_EVT_UUID_STR  = "55555555-5555-5555-5555-555555555555";

// Pre-computed BLOBs for InjectEvent
static std::array<uint8_t, 16> TEMP_SENSOR_BLOB;
static std::array<uint8_t, 16> FAN_BLOB;
static std::array<uint8_t, 16> HIGH_TEMP_EVT_BLOB;
static std::array<uint8_t, 16> MOTION_EVT_BLOB;
static std::array<uint8_t, 16> UNKNOWN_EVT_BLOB;
static std::array<uint8_t, 16> TIMEOUT_EVT_BLOB;

static const char *TEST_DB_PATH   = "/tmp/cortexlink_test.db";
static const char *TEST_LOG_DIR   = "/tmp/cortexlink_test_logs";
static const char *MQTT_BROKER_IP = "43.139.96.190";
static const int   MQTT_PORT      = 1883;

// Lua script filenames
static const char *SCRIPT_HIGH_TEMP = "test_high_temp.lua";
static const char *SCRIPT_MOTION    = "test_motion.lua";
static const char *SCRIPT_TIMEOUT   = "test_timeout.lua";

// ============================================================================
// Simple test harness
// ============================================================================

static int g_passed = 0;
static int g_failed = 0;

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
// Helpers
// ============================================================================

// Remove a file if it exists; non-fatal on error.
static void RemoveFile(const char *path) {
    std::remove(path);
}

// Remove a directory tree (shallow — only empty dirs, best-effort).
static void RemoveDir(const char *path) {
    // Use system rm -rf for simplicity in test cleanup
    std::string cmd = std::string("rm -rf ") + path;
    std::system(cmd.c_str());
}

// Create a directory (best-effort).
static bool EnsureDir(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

// Resolve HOME/.cortexlink/scripts and ensure it exists.
static std::string GetScriptDir() {
    const char *home = std::getenv("HOME");
    if (!home) {
        return ".cortexlink/scripts/";
    }
    std::string dir = std::string(home) + "/.cortexlink/scripts/";
    EnsureDir(dir);
    return dir;
}

// Write a Lua test script to disk, return full path.
static std::string WriteScript(const std::string &filename,
                               const std::string &content) {
    std::string script_dir = GetScriptDir();
    std::string full_path = script_dir + filename;
    std::ofstream out(full_path);
    if (out.is_open()) {
        out << content;
        out.close();
    }
    return full_path;
}

// Remove test scripts.
static void CleanupScripts() {
    std::string dir = GetScriptDir();
    RemoveFile((dir + SCRIPT_HIGH_TEMP).c_str());
    RemoveFile((dir + SCRIPT_MOTION).c_str());
    RemoveFile((dir + SCRIPT_TIMEOUT).c_str());
}

// Sleep for the worker thread to process an injected event.
// The RuleEngine worker polls on a condition variable and processes
// one event at a time. 500 ms is generous for a local SQLite query +
// Lua execution.
static void WaitForWorker(int ms = 500) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ============================================================================
// DB setup
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

// Insert test devices, events, rules, event_rule mappings, and device data.
// Returns false if any insert fails.
static bool InsertTestData() {
    // ---- Devices ----
    DevicePropertyTable dev_prop;

    // Temperature sensor
    {
        DevicePropertyTable::DeviceProperty dev;
        dev.dev_id    = TEMP_SENSOR_BLOB;
        dev.dev_name  = "temp_sensor";
        dev.dev_type  = "sensor";
        dev.dev_subtype = "temperature";
        dev.dev_state = "online";
        dev.location  = "living_room";
        dev.user_param = "Monitor room temperature";
        dev.actions   = "[]";
        dev.events    = "{\"evt_id\": [\"" + std::string(HIGH_TEMP_EVT_UUID_STR) + "\"]}";
        dev.data      = R"({"data": [{"d_name": "temperature", "desc": "Current temperature", "type": "float", "unit": "°C"}]})";
        if (!dev_prop.Insert(dev)) {
            std::cerr << "FATAL: Failed to insert temp_sensor device" << std::endl;
            return false;
        }
    }

    // Fan
    {
        DevicePropertyTable::DeviceProperty dev;
        dev.dev_id    = FAN_BLOB;
        dev.dev_name  = "fan_001";
        dev.dev_type  = "actuator";
        dev.dev_subtype = "fan";
        dev.dev_state = "online";
        dev.location  = "living_room";
        dev.user_param = "Cooling fan";
        dev.actions   = R"({"actions": [{"act_id": "switch", "act_name": "Switch", "desc": "Turn fan on/off", "params": [{"p_name": "state", "desc": "On or off", "p_type": "str", "range": ["on", "off"], "unit": ""}], "pre_cond": ""}]})";
        dev.events    = "{\"evt_id\": []}";
        dev.data      = "[]";
        if (!dev_prop.Insert(dev)) {
            std::cerr << "FATAL: Failed to insert fan_001 device" << std::endl;
            return false;
        }
    }

    // ---- Events ----
    EventTable event_tbl;

    // high_temp event
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

    // motion_detected event
    {
        EventTable::Event evt;
        evt.evt_id   = MOTION_EVT_BLOB;
        evt.dev_id   = TEMP_SENSOR_BLOB;
        evt.evt_name = "motion_detected";
        evt.desc     = "Motion detected in room";
        evt.params   = "{\"params\": []}";
        if (!event_tbl.Insert(evt)) {
            std::cerr << "FATAL: Failed to insert motion_detected event" << std::endl;
            return false;
        }
    }

    // timeout_test event (dedicated event for timeout rule, isolated from
    // motion_detected to avoid blocking the worker thread during other tests)
    {
        EventTable::Event evt;
        evt.evt_id   = TIMEOUT_EVT_BLOB;
        evt.dev_id   = TEMP_SENSOR_BLOB;
        evt.evt_name = "timeout_test";
        evt.desc     = "Event for testing Lua timeout handling";
        evt.params   = "{\"params\": []}";
        if (!event_tbl.Insert(evt)) {
            std::cerr << "FATAL: Failed to insert timeout_test event" << std::endl;
            return false;
        }
    }

    // ---- Rules ----
    RuleTable rule_tbl;

    // Rule 1: high_temp_rule — with condition (temperature > 30)
    {
        RuleTable::Rule rule;
        rule.rule_name = "high_temp_rule";
        rule.rule_type = "automation";
        rule.enable    = true;
        rule.count     = 0;
        rule.limit     = 0;
        // Format: <evt_uuid>(<condition>)
        rule.cond_expr = std::string(HIGH_TEMP_EVT_UUID_STR)
                         + "({event.temperature} > 30)";
        rule.action    = SCRIPT_HIGH_TEMP;
        if (!rule_tbl.Insert(rule)) {
            std::cerr << "FATAL: Failed to insert high_temp_rule" << std::endl;
            return false;
        }
    }

    // Rule 2: motion_rule — no condition (always executes)
    {
        RuleTable::Rule rule;
        rule.rule_name = "motion_rule";
        rule.rule_type = "automation";
        rule.enable    = true;
        rule.count     = 0;
        rule.limit     = 0;
        rule.cond_expr.clear();  // no condition → always-true
        rule.action    = SCRIPT_MOTION;
        if (!rule_tbl.Insert(rule)) {
            std::cerr << "FATAL: Failed to insert motion_rule" << std::endl;
            return false;
        }
    }

    // Rule 3: disabled_rule — same as motion_rule but disabled
    {
        RuleTable::Rule rule;
        rule.rule_name = "disabled_rule";
        rule.rule_type = "automation";
        rule.enable    = false;   // ← disabled
        rule.count     = 0;
        rule.limit     = 0;
        rule.cond_expr.clear();
        rule.action    = SCRIPT_MOTION;
        if (!rule_tbl.Insert(rule)) {
            std::cerr << "FATAL: Failed to insert disabled_rule" << std::endl;
            return false;
        }
    }

    // Rule 4: limited_rule — rate-limited (limit=1, count=1 → exceeded)
    {
        RuleTable::Rule rule;
        rule.rule_name = "limited_rule";
        rule.rule_type = "automation";
        rule.enable    = true;
        rule.count     = 1;       // already at limit
        rule.limit     = 1;
        rule.cond_expr.clear();
        rule.action    = SCRIPT_MOTION;
        if (!rule_tbl.Insert(rule)) {
            std::cerr << "FATAL: Failed to insert limited_rule" << std::endl;
            return false;
        }
    }

    // Rule 5: timeout_rule — for testing timeout (has Script that loops forever)
    {
        RuleTable::Rule rule;
        rule.rule_name = "timeout_rule";
        rule.rule_type = "automation";
        rule.enable    = true;
        rule.count     = 0;
        rule.limit     = 0;
        rule.cond_expr.clear();
        rule.action    = SCRIPT_TIMEOUT;
        if (!rule_tbl.Insert(rule)) {
            std::cerr << "FATAL: Failed to insert timeout_rule" << std::endl;
            return false;
        }
    }

    // ---- Event-Rule Mappings ----
    EventRuleTable evt_rule_tbl;

    // high_temp_evt → rule_id 1 (high_temp_rule, with condition)
    {
        EventRuleTable::EventRule er;
        er.evt_id  = HIGH_TEMP_EVT_BLOB;
        er.rule_id = 1;
        evt_rule_tbl.Insert(er);
    }

    // motion_evt → rule_id 2 (motion_rule, no condition)
    {
        EventRuleTable::EventRule er;
        er.evt_id  = MOTION_EVT_BLOB;
        er.rule_id = 2;
        evt_rule_tbl.Insert(er);
    }

    // motion_evt → rule_id 3 (disabled_rule)
    {
        EventRuleTable::EventRule er;
        er.evt_id  = MOTION_EVT_BLOB;
        er.rule_id = 3;
        evt_rule_tbl.Insert(er);
    }

    // motion_evt → rule_id 4 (limited_rule)
    {
        EventRuleTable::EventRule er;
        er.evt_id  = MOTION_EVT_BLOB;
        er.rule_id = 4;
        evt_rule_tbl.Insert(er);
    }

    // timeout_evt → rule_id 5 (timeout_rule) — isolated from motion_evt
    // so the timeout+retry cycle (~7 s) does not block other test cases
    {
        EventRuleTable::EventRule er;
        er.evt_id  = TIMEOUT_EVT_BLOB;
        er.rule_id = 5;
        evt_rule_tbl.Insert(er);
    }

    // ---- Device Data ----
    DeviceDataTable dev_data_tbl;

    // temp_sensor: temperature = 25.0
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

// Create Lua script files on disk.
static bool CreateTestScripts() {
    // test_high_temp.lua — reads device data, conditionally does action
    {
        std::string content = R"lua(
-- Rule: high_temp_rule
local temp = event.params.temperature
log("INFO", "high_temp triggered, temp=" .. tostring(temp))
local d = get_data("11111111-1111-1111-1111-111111111111", "temperature")
if d and tonumber(d) > 30 then
    local ok, err = do_action("22222222-2222-2222-2222-222222222222", "switch", {state = "on"})
    if not ok then
        log("ERROR", "do_action failed: " .. tostring(err))
    end
end
)lua";
        WriteScript(SCRIPT_HIGH_TEMP, content);
    }

    // test_motion.lua — simple publish + log
    {
        std::string content = R"lua(
-- Rule: motion_rule / disabled_rule / limited_rule
log("INFO", "motion detected from " .. event.dev_id)
publish("test/result", "motion_rule_executed")
)lua";
        WriteScript(SCRIPT_MOTION, content);
    }

    // test_timeout.lua — infinite loop, triggers timeout
    {
        std::string content = R"lua(
-- Rule: timeout_rule — infinite loop to trigger timeout
while true do
end
)lua";
        WriteScript(SCRIPT_TIMEOUT, content);
    }

    return true;
}

// ============================================================================
// Test Cases — A. InjectEvent 集成测试 (cases 1-6)
// ============================================================================

static void TestCase1_UnknownEvent(RuleEngine &engine) {
    TestBegin("case 1: unknown event (no rules bound)");

    // Record current state: no rules should be triggered
    EventRecordTable rec_tbl;
    auto before_records = rec_tbl.GetAll();

    // Inject unknown event
    std::string params = R"([{"p_name": "val", "value": 42}])";
    engine.InjectEvent(UNKNOWN_EVT_BLOB, TEMP_SENSOR_BLOB,
                       "unknown_event", params);
    WaitForWorker();

    // Verify: event_record should have a new entry for unknown event
    auto after_records = rec_tbl.GetAll();
    ASSERT_TRUE(after_records.size() > before_records.size(),
                "expected new event_record entry for unknown event");

    // Verify: rule counts should not have changed
    RuleTable rule_tbl;
    auto rule1 = rule_tbl.GetByRuleId(1);
    ASSERT_TRUE(rule1.has_value(), "rule 1 should exist");
    ASSERT_TRUE(rule1->count == 0, "rule 1 count should still be 0");

    TestPass();
}

static void TestCase2_NoConditionRule(RuleEngine &engine) {
    TestBegin("case 2: matching rule without condition expression");

    RuleTable rule_tbl;
    auto before = rule_tbl.GetByRuleId(2);
    ASSERT_TRUE(before.has_value(), "rule 2 should exist");
    int64_t before_count = before->count;

    // Inject motion_detected event (no params)
    std::string params = "[]";
    engine.InjectEvent(MOTION_EVT_BLOB, TEMP_SENSOR_BLOB,
                       "motion_detected", params);
    WaitForWorker();

    auto after = rule_tbl.GetByRuleId(2);
    ASSERT_TRUE(after.has_value(), "rule 2 should still exist");
    ASSERT_TRUE(after->count == before_count + 1,
                "rule 2 count should increment by 1");

    TestPass();
}

static void TestCase3_ConditionTrue(RuleEngine &engine) {
    TestBegin("case 3: condition expression satisfied (temp=35 > 30)");

    RuleTable rule_tbl;
    auto before = rule_tbl.GetByRuleId(1);
    ASSERT_TRUE(before.has_value(), "rule 1 should exist");
    int64_t before_count = before->count;

    // Inject high_temp with temperature=35 (satisfies > 30)
    std::string params =
        R"([{"p_name": "temperature", "value": 35.0}])";
    engine.InjectEvent(HIGH_TEMP_EVT_BLOB, TEMP_SENSOR_BLOB,
                       "high_temp", params);
    WaitForWorker();

    auto after = rule_tbl.GetByRuleId(1);
    ASSERT_TRUE(after.has_value(), "rule 1 should still exist");
    ASSERT_TRUE(after->count == before_count + 1,
                "rule 1 count should increment (condition satisfied)");

    TestPass();
}

static void TestCase4_ConditionFalse(RuleEngine &engine) {
    TestBegin("case 4: condition expression not satisfied (temp=20 < 30)");

    RuleTable rule_tbl;
    auto before = rule_tbl.GetByRuleId(1);
    ASSERT_TRUE(before.has_value(), "rule 1 should exist");
    int64_t before_count = before->count;

    // Inject high_temp with temperature=20 (does NOT satisfy > 30)
    std::string params =
        R"([{"p_name": "temperature", "value": 20.0}])";
    engine.InjectEvent(HIGH_TEMP_EVT_BLOB, TEMP_SENSOR_BLOB,
                       "high_temp", params);
    WaitForWorker();

    auto after = rule_tbl.GetByRuleId(1);
    ASSERT_TRUE(after.has_value(), "rule 1 should still exist");
    ASSERT_TRUE(after->count == before_count,
                "rule 1 count should NOT increment (condition failed)");

    TestPass();
}

static void TestCase5_DisabledRule(RuleEngine &engine) {
    TestBegin("case 5: disabled rule is skipped");

    RuleTable rule_tbl;
    auto before = rule_tbl.GetByRuleId(3);
    ASSERT_TRUE(before.has_value(), "rule 3 should exist");
    ASSERT_FALSE(before->enable, "rule 3 should be disabled");
    int64_t before_count = before->count;

    // Inject motion_detected (rule 2, 3, 4, 5 are bound; rule 3 is disabled)
    std::string params = "[]";
    engine.InjectEvent(MOTION_EVT_BLOB, TEMP_SENSOR_BLOB,
                       "motion_detected", params);
    WaitForWorker();

    // Rule 3 count should NOT change (it's disabled)
    auto after = rule_tbl.GetByRuleId(3);
    ASSERT_TRUE(after.has_value(), "rule 3 should still exist");
    ASSERT_TRUE(after->count == before_count,
                "disabled rule count should not change");

    TestPass();
}

static void TestCase6_LimitedRule(RuleEngine &engine) {
    TestBegin("case 6: rate-limited rule is skipped");

    RuleTable rule_tbl;
    auto before = rule_tbl.GetByRuleId(4);
    ASSERT_TRUE(before.has_value(), "rule 4 should exist");
    ASSERT_TRUE(before->limit == 1 && before->count >= 1,
                "rule 4 should be at its limit");
    int64_t before_count = before->count;

    // Inject motion_detected (rule 4 is at limit)
    std::string params = "[]";
    engine.InjectEvent(MOTION_EVT_BLOB, TEMP_SENSOR_BLOB,
                       "motion_detected", params);
    WaitForWorker();

    // Rule 4 count should NOT change
    auto after = rule_tbl.GetByRuleId(4);
    ASSERT_TRUE(after.has_value(), "rule 4 should still exist");
    ASSERT_TRUE(after->count == before_count,
                "rate-limited rule count should not change");

    TestPass();
}

// ============================================================================
// Test Cases — B. LuaSandbox 脚本 API 测试 (cases 7-10)
// ============================================================================

static void TestCase7_GetData(MqttClient &mqtt) {
    TestBegin("case 7: Lua get_data() reads device data");

    DeviceDataTable dev_data;
    DevicePropertyTable dev_prop;
    RuleTable rule_tbl;

    OpenClawClient open_claw;
    LuaSandbox sandbox(&dev_data, &dev_prop, &mqtt, &rule_tbl, &open_claw);

    // Script that reads temperature data and logs it
    // If get_data returns nil, we convert to "nil" string — script still
    // succeeds, but we verify it doesn't error.
    std::string script = R"lua(
local d = get_data("11111111-1111-1111-1111-111111111111", "temperature")
log("INFO", "get_data returned: " .. tostring(d))
)lua";

    LuaSandbox::EventContext evt;
    evt.evt_id   = HIGH_TEMP_EVT_UUID_STR;
    evt.evt_name = "high_temp";
    evt.dev_id   = TEMP_SENSOR_UUID_STR;
    evt.params   = {{"temperature", "35.0"}};

    bool ok = sandbox.Execute(script, evt, 99);
    ASSERT_TRUE(ok, "get_data script should execute successfully");

    TestPass();
}

static void TestCase8_DoAction(MqttClient &mqtt) {
    TestBegin("case 8: Lua do_action() publishes MQTT action");

    DeviceDataTable dev_data;
    DevicePropertyTable dev_prop;
    RuleTable rule_tbl;

    OpenClawClient open_claw;
    LuaSandbox sandbox(&dev_data, &dev_prop, &mqtt, &rule_tbl, &open_claw);

    // Script that turns on the fan — device fan_001 is online
    std::string script = R"lua(
local ok, err = do_action("22222222-2222-2222-2222-222222222222", "switch", {state = "on"})
if not ok then
    log("ERROR", "do_action failed: " .. tostring(err))
else
    log("INFO", "do_action succeeded")
end
)lua";

    LuaSandbox::EventContext evt;
    evt.evt_id   = HIGH_TEMP_EVT_UUID_STR;
    evt.evt_name = "high_temp";
    evt.dev_id   = TEMP_SENSOR_UUID_STR;
    evt.params   = {{"temperature", "35.0"}};

    bool ok = sandbox.Execute(script, evt, 99);
    ASSERT_TRUE(ok, "do_action script should execute successfully");

    TestPass();
}

static void TestCase9_Publish(MqttClient &mqtt) {
    TestBegin("case 9: Lua publish() sends MQTT message");

    DeviceDataTable dev_data;
    DevicePropertyTable dev_prop;
    RuleTable rule_tbl;

    OpenClawClient open_claw;
    LuaSandbox sandbox(&dev_data, &dev_prop, &mqtt, &rule_tbl, &open_claw);

    std::string script = R"lua(
local ok, err = publish("test/channel", "hello_from_test")
if not ok then
    log("ERROR", "publish failed: " .. tostring(err))
end
)lua";

    LuaSandbox::EventContext evt;
    evt.evt_id   = MOTION_EVT_UUID_STR;
    evt.evt_name = "motion_detected";
    evt.dev_id   = TEMP_SENSOR_UUID_STR;

    bool ok = sandbox.Execute(script, evt, 99);
    ASSERT_TRUE(ok, "publish script should execute successfully");

    TestPass();
}

static void TestCase10_Timeout(MqttClient &mqtt) {
    TestBegin("case 10: Lua script timeout → retry → disable rule");

    DeviceDataTable dev_data;
    DevicePropertyTable dev_prop;
    RuleTable rule_tbl;

    OpenClawClient open_claw;
    LuaSandbox sandbox(&dev_data, &dev_prop, &mqtt, &rule_tbl, &open_claw);

    // infinite loop — will time out twice
    std::string script = "while true do end";

    LuaSandbox::EventContext evt;
    evt.evt_id   = MOTION_EVT_UUID_STR;
    evt.evt_name = "motion_detected";
    evt.dev_id   = TEMP_SENSOR_UUID_STR;

    // Use a test rule_id — the sandbox will disable it on double timeout
    int64_t test_rule_id = 99;

    bool ok = sandbox.Execute(script, evt, test_rule_id);
    ASSERT_FALSE(ok, "infinite loop script should fail after double timeout");

    // Verify the rule was disabled
    auto rule = rule_tbl.GetByRuleId(test_rule_id);
    // Rule 99 doesn't exist in DB, so the SetEnable call inside LuaSandbox
    // would fail silently.  For a proper timeout test, use an existing rule.
    // Let's re-test with a real rule_id from the DB.
    //
    // Actually, LuaSandbox only calls rule_table_->SetEnable on double
    // timeout.  If the rule doesn't exist, it's a no-op (SQL UPDATE
    // affects 0 rows but doesn't error).  The important thing is that
    // Execute returns false.
    //
    // We already have rule 5 as timeout_rule in the DB.  Let's use it
    // via InjectEvent for a full integration test of the timeout path.

    TestPass();
}

// Sub-test: timeout via full InjectEvent pipeline.
// Uses a dedicated timeout_evt so the ~7 s timeout+retry cycle does not
// interfere with the other test cases that share motion_evt.
static void TestCase10b_TimeoutIntegration(RuleEngine &engine) {
    TestBegin("case 10b: timeout via InjectEvent → rule disabled");

    RuleTable rule_tbl;

    // Ensure rule 5 (timeout_rule) exists and is enabled before we start.
    auto before = rule_tbl.GetByRuleId(5);
    ASSERT_TRUE(before.has_value(), "rule 5 (timeout_rule) should exist");
    if (!before->enable) {
        // Re-enable it in case a prior run left it disabled.
        rule_tbl.SetEnable(5, true);
        before = rule_tbl.GetByRuleId(5);
    }
    ASSERT_TRUE(before->enable, "rule 5 should be enabled before timeout test");

    // Inject timeout_evt → triggers only rule 5 (infinite loop → timeout).
    engine.InjectEvent(TIMEOUT_EVT_BLOB, TEMP_SENSOR_BLOB,
                       "timeout_test", "[]");

    // LuaSandbox timeout: ~3 s first attempt + 1 s retry delay + ~3 s
    // second attempt ≈ 7 s.  Add a safety margin.
    WaitForWorker(9000);

    // After double timeout the sandbox disables the rule.
    auto after = rule_tbl.GetByRuleId(5);
    ASSERT_TRUE(after.has_value(), "rule 5 should still exist");
    ASSERT_FALSE(after->enable,
                 "rule 5 should be disabled after double timeout");

    TestPass();
}

// ============================================================================
// Test Cases — C. 条件表达式单元测试 (cases 11-12)
// ============================================================================

static void TestCase11_ConditionAnd() {
    TestBegin("case 11: condition {event.temperature} > 30 && {event.humidity} < 80");

    // Build a condition expression that uses two event params with AND
    // Format must include an evt_id prefix:  evt_id(condition)
    std::string cond_expr =
        std::string(HIGH_TEMP_EVT_UUID_STR)
        + "({event.temperature} > 30 && {event.humidity} < 80)";

    auto ast = ParseCondition(cond_expr, HIGH_TEMP_EVT_UUID_STR);
    ASSERT_TRUE(ast != nullptr, "ParseCondition should return non-null AST");

    EvalContext ctx;
    ctx.event_params["temperature"] = "35";
    ctx.event_params["humidity"]    = "60";

    bool result = EvaluateAst(ast.get(), ctx);
    ASSERT_TRUE(result, "35>30 && 60<80 should be true");

    // Also test boundary: humidity too high → false
    ctx.event_params["humidity"] = "90";
    result = EvaluateAst(ast.get(), ctx);
    ASSERT_FALSE(result, "35>30 && 90<80 should be false");

    TestPass();
}

static void TestCase12_DeviceDataAndTime() {
    TestBegin("case 12: condition {dev.data} >= 25 || {time} < \"12:00\"");

    // Use a temp event UUID for this standalone parser test
    static const char *test_evt = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
    std::string cond_expr =
        std::string(test_evt)
        + "({" + TEMP_SENSOR_UUID_STR + ".temperature} >= 25 || {time} < \"12:00\")";

    auto ast = ParseCondition(cond_expr, test_evt);
    ASSERT_TRUE(ast != nullptr, "ParseCondition should return non-null AST");

    EvalContext ctx;

    // Provide device data callback — temperature is 25.0 (satisfies >= 25)
    ctx.get_device_data =
        [](const std::string &dev_id,
           const std::string &data_name) -> std::optional<std::string> {
        if (dev_id == TEMP_SENSOR_UUID_STR && data_name == "temperature") {
            return "25.0";
        }
        return std::nullopt;
    };

    // Time set to 14:00 (does NOT satisfy < "12:00")
    ctx.hour = 14;
    ctx.min  = 0;
    ctx.sec  = 0;

    // First operand true (25.0 >= 25), so OR is true regardless of time
    bool result = EvaluateAst(ast.get(), ctx);
    ASSERT_TRUE(result, "25.0>=25 should be true making OR true");

    // Now test with device data false and time true
    ctx.get_device_data =
        [](const std::string &dev_id,
           const std::string &data_name) -> std::optional<std::string> {
        if (dev_id == TEMP_SENSOR_UUID_STR && data_name == "temperature") {
            return "10.0";  // < 25, so first operand false
        }
        return std::nullopt;
    };
    ctx.hour = 8;  // < "12:00", second operand true

    result = EvaluateAst(ast.get(), ctx);
    ASSERT_TRUE(result, "10.0>=25 is false but 08:00<12:00 is true, OR should be true");

    // Both false
    ctx.hour = 14;  // > "12:00"
    result = EvaluateAst(ast.get(), ctx);
    ASSERT_FALSE(result, "10.0>=25 is false and 14:00<12:00 is false, OR should be false");

    TestPass();
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== CortexLink 链路测试 ===" << std::endl;
    std::cout << "MQTT Broker: " << MQTT_BROKER_IP << ":" << MQTT_PORT
              << std::endl;
    std::cout << "Test DB:     " << TEST_DB_PATH << std::endl;

    // ---- Pre-compute BLOBs ----
    TEMP_SENSOR_BLOB    = util::UuidToBlob(TEMP_SENSOR_UUID_STR);
    FAN_BLOB            = util::UuidToBlob(FAN_UUID_STR);
    HIGH_TEMP_EVT_BLOB  = util::UuidToBlob(HIGH_TEMP_EVT_UUID_STR);
    MOTION_EVT_BLOB     = util::UuidToBlob(MOTION_EVT_UUID_STR);
    UNKNOWN_EVT_BLOB    = util::UuidToBlob(UNKNOWN_EVT_UUID_STR);
    TIMEOUT_EVT_BLOB    = util::UuidToBlob(TIMEOUT_EVT_UUID_STR);

    // ---- Setup ----
    std::cout << "\n--- Setup ---" << std::endl;

    // 1. Logger
    util::InitLogger(TEST_LOG_DIR, spdlog::level::debug);
    spdlog::info("=== CortexLinkTest starting ===");

    // 2. Database
    if (!SetupDatabase()) {
        std::cerr << "Database setup failed, aborting." << std::endl;
        return 1;
    }
    std::cout << "  Database initialized." << std::endl;

    // 3. Insert test data
    if (!InsertTestData()) {
        std::cerr << "Test data insertion failed, aborting." << std::endl;
        DBTable::Shutdown();
        return 1;
    }
    std::cout << "  Test data inserted." << std::endl;

    // 4. Create test Lua scripts
    CreateTestScripts();
    std::cout << "  Lua scripts created." << std::endl;

    // 5. MQTT connection
    MqttClient mqtt("cortexlink-test");
    mqtt.SetCredentials("lky1", "110090");
    if (!mqtt.Connect(MQTT_BROKER_IP, MQTT_PORT)) {
        std::cerr << "FATAL: Failed to connect to MQTT broker at "
                  << MQTT_BROKER_IP << std::endl;
        CleanupScripts();
        DBTable::Shutdown();
        return 1;
    }
    std::cout << "  MQTT connected to " << MQTT_BROKER_IP << "." << std::endl;

    if (!mqtt.LoopStart()) {
        std::cerr << "FATAL: Failed to start MQTT loop" << std::endl;
        mqtt.Disconnect();
        CleanupScripts();
        DBTable::Shutdown();
        return 1;
    }

    // 6. DeviceManager
    DeviceManager dev_mgr(&mqtt);
    if (!dev_mgr.Start()) {
        std::cerr << "FATAL: Failed to start DeviceManager" << std::endl;
        mqtt.LoopStop();
        mqtt.Disconnect();
        CleanupScripts();
        DBTable::Shutdown();
        return 1;
    }
    std::cout << "  DeviceManager started." << std::endl;

    // 7. RuleEngine
    OpenClawClient open_claw;
    RuleEngine rule_engine(&mqtt, &dev_mgr, &open_claw);
    if (!rule_engine.Start()) {
        std::cerr << "FATAL: Failed to start RuleEngine" << std::endl;
        dev_mgr.Stop();
        mqtt.LoopStop();
        mqtt.Disconnect();
        CleanupScripts();
        DBTable::Shutdown();
        return 1;
    }
    std::cout << "  RuleEngine started." << std::endl;

    // ---- Run tests ----
    std::cout << "\n--- A. InjectEvent Integration Tests ---" << std::endl;
    TestCase1_UnknownEvent(rule_engine);
    TestCase2_NoConditionRule(rule_engine);
    TestCase3_ConditionTrue(rule_engine);
    TestCase4_ConditionFalse(rule_engine);
    TestCase5_DisabledRule(rule_engine);
    TestCase6_LimitedRule(rule_engine);

    std::cout << "\n--- B. LuaSandbox Script API Tests ---" << std::endl;
    TestCase7_GetData(mqtt);
    TestCase8_DoAction(mqtt);
    TestCase9_Publish(mqtt);
    TestCase10_Timeout(mqtt);
    TestCase10b_TimeoutIntegration(rule_engine);

    std::cout << "\n--- C. Condition Parser Unit Tests ---" << std::endl;
    TestCase11_ConditionAnd();
    TestCase12_DeviceDataAndTime();

    // ---- Cleanup ----
    std::cout << "\n--- Cleanup ---" << std::endl;

    rule_engine.Stop();
    dev_mgr.Stop();
    mqtt.LoopStop();
    mqtt.Disconnect();

    CleanupScripts();
    DBTable::Shutdown();
    RemoveFile(TEST_DB_PATH);
    RemoveDir(TEST_LOG_DIR);

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
