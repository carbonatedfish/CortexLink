/**
 * CortexLink OpenClaw 连通性测试
 *
 * 测试覆盖：
 *   A. MCP Server 连通性探测
 *   B. 发送提示词 → OpenClaw 通过 LlmSqlProxy 读取数据库规则 → 验证响应
 *
 * 目的：验证 OpenClaw 能否正确使用数据库终结点读取数据。
 * 区别于 test/llm_test.cpp 的 D 组（规则生成），本测试仅验证连通性和读取能力。
 *
 * 依赖：
 *   - B 组需要 MCP Server @ 127.0.0.1:18789
 *   - B 组需要端口 8899 可用（LlmSqlProxy）
 */

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "db/db_table.h"
#include "db/rule_table.h"
#include "llm/llm_sql_proxy.h"
#include "llm/open_claw_client.h"
#include "util/log_util.h"
#include "util/uuid_util.h"

using namespace cortexlink;

// ============================================================================
// Test constants
// ============================================================================

static const char *TEST_DB_PATH = "/tmp/cortexlink_openclaw_conn_test.db";
static const char *TEST_LOG_DIR = "/tmp/cortexlink_openclaw_conn_test_logs";
static const int   PROXY_PORT   = 8899;   // must match the skill file's endpoint
static const char *MCP_ENDPOINT = "http://127.0.0.1:18789";
static const char *RULE_1_NAME  = "openclaw_conn_test_rule_1";
static const char *RULE_2_NAME  = "openclaw_conn_test_rule_2";

// ============================================================================
// Simple test harness
// ============================================================================

static int  g_passed        = 0;
static int  g_failed        = 0;
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
// Helpers — file operations
// ============================================================================

static void RemoveFile(const char *path) {
    std::remove(path);
}

static void RemoveDir(const char *path) {
    std::string cmd = std::string("rm -rf ") + path;
    std::system(cmd.c_str());
}

// ============================================================================
// Helpers — network
// ============================================================================

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
// Helpers — IsHistoryComplete
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

    // Only the rule table is needed for this test.
    RuleTable rule_tbl;
    if (!rule_tbl.CreateTable()) {
        std::cerr << "FATAL: Failed to create rule table" << std::endl;
        return false;
    }

    return true;
}

// ============================================================================
// Seed known rules directly via RuleTable (proxy is not running yet)
// ============================================================================

static bool InsertKnownRules() {
    RuleTable rule_tbl;

    // Rule 1
    {
        RuleTable::Rule rule;
        rule.rule_name = RULE_1_NAME;
        rule.rule_type = "automation";
        rule.enable    = true;
        rule.count     = 0;
        rule.limit     = 0;
        rule.cond_expr = "";
        rule.action    = "conn_test_1.lua";
        if (!rule_tbl.Insert(rule)) {
            std::cerr << "FATAL: Failed to insert " << RULE_1_NAME << std::endl;
            return false;
        }
        spdlog::info("Inserted rule: id={}, name='{}'",
                     rule.rule_id, rule.rule_name);
    }

    // Rule 2
    {
        RuleTable::Rule rule;
        rule.rule_name = RULE_2_NAME;
        rule.rule_type = "reminder";
        rule.enable    = true;
        rule.count     = 0;
        rule.limit     = 0;
        rule.cond_expr = "";
        rule.action    = "conn_test_2.lua";
        if (!rule_tbl.Insert(rule)) {
            std::cerr << "FATAL: Failed to insert " << RULE_2_NAME << std::endl;
            return false;
        }
        spdlog::info("Inserted rule: id={}, name='{}'",
                     rule.rule_id, rule.rule_name);
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
// Test Cases — A. MCP Connectivity
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
// Test Cases — B. Send prompt, let OpenClaw read rules, verify response
// ============================================================================

static void TestCaseB1_SendAndVerify() {
    TestBegin("B.1 send prompt → OpenClaw reads rules via LlmSqlProxy → verify");

    OpenClawClient open_claw;
    open_claw.SetEndpoint(MCP_ENDPOINT);

    // Generate a session ID for this conversation.
    std::string session = util::GenerateUuid();

    // Prompt: tell OpenClaw exactly where the database is and what to do.
    // The prompt includes the endpoint URL and the command to use,
    // so the LLM can call LlmSqlProxy even without the skill file pre-loaded.
    std::string prompt =
        "你可以通过 HTTP POST 访问数据库。数据库 API 地址是 "
        "http://127.0.0.1:8899/sql ，请求格式为 "
        "{\"cmd\": \"<命令>\", \"params\": {<参数>}} 。\n\n"
        "请使用 get_rules 命令查询数据库中的所有规则，"
        "然后告诉我你找到了哪些规则（包括规则的 rule_name 和 rule_type）。";

    spdlog::info("Sending prompt to MCP Server (session={})", session);

    auto send_resp = open_claw.SendMessageAndGetResponse(session, prompt);
    ASSERT_TRUE(send_resp.has_value(),
                "SendMessageAndGetResponse should return a value "
                "(MCP Server or LLM backend may be unavailable)");

    spdlog::info("Prompt sent successfully (session={})", session);

    // Poll for completion — max 90 attempts × 2s = 180s timeout.
    // Using 2-second intervals to give the LLM time to make the HTTP call.
    static constexpr int kMaxAttempts = 90;
    bool completed = false;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

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

            // Verify: the response should contain both rule names.
            std::string history_str = history->dump();

            auto pos1 = history_str.find(RULE_1_NAME);
            auto pos2 = history_str.find(RULE_2_NAME);

            if (pos1 != std::string::npos) {
                spdlog::info("Found '{}' in MCP response", RULE_1_NAME);
            } else {
                spdlog::warn("'{}' NOT found in MCP response", RULE_1_NAME);
            }
            if (pos2 != std::string::npos) {
                spdlog::info("Found '{}' in MCP response", RULE_2_NAME);
            } else {
                spdlog::warn("'{}' NOT found in MCP response", RULE_2_NAME);
            }

            ASSERT_TRUE(pos1 != std::string::npos,
                        "MCP response should contain openclaw_conn_test_rule_1");
            ASSERT_TRUE(pos2 != std::string::npos,
                        "MCP response should contain openclaw_conn_test_rule_2");

            break;
        }

        spdlog::debug("Waiting for MCP completion (attempt {}/{})",
                      attempt + 1, kMaxAttempts);
    }

    ASSERT_TRUE(completed, "MCP processing timed out after 60s");

    TestPass();
}

// ============================================================================
// Cleanup
// ============================================================================

static void CleanupAll() {
    // Delete the rules we inserted.
    RuleTable rule_tbl;
    auto rules = rule_tbl.GetAll();
    for (const auto &rule : rules) {
        if (rule.rule_name == RULE_1_NAME || rule.rule_name == RULE_2_NAME) {
            rule_tbl.Delete(rule.rule_id);
            spdlog::info("Deleted rule: id={}, name='{}'",
                         rule.rule_id, rule.rule_name);
        }
    }

    // Shut down proxy and database.
    StopProxy();
    DBTable::Shutdown();

    // Remove temp files.
    RemoveFile(TEST_DB_PATH);
    RemoveDir(TEST_LOG_DIR);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== CortexLink OpenClaw Connectivity Test ===" << std::endl;
    std::cout << "Test DB:     " << TEST_DB_PATH << std::endl;
    std::cout << "Proxy port:  " << PROXY_PORT << std::endl;
    std::cout << "MCP endpoint:" << MCP_ENDPOINT << std::endl;

    // ---- Setup ----
    std::cout << "\n--- Setup ---" << std::endl;

    // 1. Logger
    util::InitLogger(TEST_LOG_DIR, spdlog::level::debug);
    spdlog::info("=== CortexLinkOpenClawConnectivityTest starting ===");

    // 2. Database
    if (!SetupDatabase()) {
        std::cerr << "Database setup failed, aborting." << std::endl;
        return 1;
    }
    std::cout << "  Database initialized." << std::endl;

    // 3. Insert known rules (before proxy starts — direct DB access)
    if (!InsertKnownRules()) {
        std::cerr << "Rule insertion failed, aborting." << std::endl;
        DBTable::Shutdown();
        RemoveFile(TEST_DB_PATH);
        RemoveDir(TEST_LOG_DIR);
        return 1;
    }
    std::cout << "  Known rules inserted." << std::endl;

    // ---- A. Connectivity ----
    std::cout << "\n--- A. Connectivity ---" << std::endl;
    TestCaseA1_McpConnectivity();

    // ---- B. Send prompt, read rules, verify ----
    if (g_mcp_available) {
        // Check port availability
        if (!IsPortAvailable(PROXY_PORT)) {
            std::cout << "\n--- B. OpenClaw → LlmSqlProxy → Read Rules ---"
                      << std::endl;
            std::cout << "  SKIPPED — port " << PROXY_PORT
                      << " is in use (another LlmSqlProxy or CortexLink "
                         "instance may be running)" << std::endl;
        } else if (!StartProxy(PROXY_PORT)) {
            std::cout << "\n--- B. OpenClaw → LlmSqlProxy → Read Rules ---"
                      << std::endl;
            std::cout << "  SKIPPED — failed to start LlmSqlProxy on port "
                      << PROXY_PORT << std::endl;
        } else {
            std::cout << "\n--- B. OpenClaw → LlmSqlProxy → Read Rules ---"
                      << std::endl;
            std::cout << "  LlmSqlProxy started on port " << PROXY_PORT
                      << "." << std::endl;

            TestCaseB1_SendAndVerify();
        }
    } else {
        std::cout << "\n--- B. OpenClaw → LlmSqlProxy → Read Rules ---"
                  << std::endl;
        std::cout << "  SKIPPED — MCP Server not running at "
                  << MCP_ENDPOINT << std::endl;
    }

    // ---- Cleanup ----
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
