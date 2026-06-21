#include "llm/llm_sql_proxy.h"

#include <chrono>
#include <ctime>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "util/uuid_util.h"

namespace cortexlink {

// ============================================================================
// LlmSqlProxy — Construction / Destruction
// ============================================================================

LlmSqlProxy::LlmSqlProxy()
    : socket_path_(SocketPathOrDefault())
{
}

LlmSqlProxy::~LlmSqlProxy()
{
    Stop();
}

// ============================================================================
// LlmSqlProxy — Path Configuration
// ============================================================================

void LlmSqlProxy::SetSocketPath(const std::string &path)
{
    socket_path_ = path;
}

std::string LlmSqlProxy::SocketPathOrDefault()
{
    const char *home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.cortexlink/llm_sql.sock";
    }
    return "/tmp/cortexlink_llm_sql.sock";
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

    running_ = true;
    accept_thread_ = std::thread(&LlmSqlProxy::AcceptLoop, this);

    // Give the thread a moment to set up the socket.
    // If it fails, running_ will be set back to false.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!running_) {
        // AcceptLoop failed and cleared the flag.
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        return false;
    }

    spdlog::info("LlmSqlProxy: started (listening on {})", socket_path_);
    return true;
}

void LlmSqlProxy::Stop()
{
    if (!running_) return;

    running_ = false;

    // Close the listen socket to unblock accept().
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    spdlog::info("LlmSqlProxy: stopped");
}

// ============================================================================
// LlmSqlProxy — AcceptLoop (runs on dedicated thread)
// ============================================================================

void LlmSqlProxy::AcceptLoop()
{
    // 1. Create socket
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        spdlog::error("LlmSqlProxy: socket() failed: {}", strerror(errno));
        running_ = false;
        return;
    }

    // 2. Unlink stale socket file from a previous run
    unlink(socket_path_.c_str());

    // 3. Bind
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
        spdlog::error("LlmSqlProxy: bind({}) failed: {}",
                      socket_path_, strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return;
    }

    // 4. Listen
    if (listen(listen_fd_, 5) < 0) {
        spdlog::error("LlmSqlProxy: listen() failed: {}", strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(socket_path_.c_str());
        running_ = false;
        return;
    }

    // 5. Restrict access to owner only
    chmod(socket_path_.c_str(), 0600);

    spdlog::info("LlmSqlProxy: listening on {}", socket_path_);

    // 6. Accept loop
    while (running_) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);

        if (!running_) break;

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            spdlog::warn("LlmSqlProxy: accept() failed: {}", strerror(errno));
            continue;
        }

        HandleClient(client_fd);
        close(client_fd);
    }

    // 7. Cleanup
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    unlink(socket_path_.c_str());
}

// ============================================================================
// LlmSqlProxy — HandleClient
// ============================================================================

void LlmSqlProxy::HandleClient(int client_fd)
{
    nlohmann::json request;
    if (!ReadRequest(client_fd, request)) {
        return;  // error response already sent
    }

    ProcessAndRespond(client_fd, request);
}

// ============================================================================
// LlmSqlProxy — ReadRequest
// ============================================================================

bool LlmSqlProxy::ReadRequest(int client_fd, nlohmann::json &request)
{
    std::string data;
    char buf[4096];
    const size_t kMaxRequest = 65536;

    while (data.size() < kMaxRequest) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::warn("LlmSqlProxy: read() failed: {}", strerror(errno));
            return false;
        }
        if (n == 0) {
            // EOF — client closed write side (or disconnected)
            break;
        }
        data.append(buf, static_cast<size_t>(n));
        if (data.find('\n') != std::string::npos) {
            break;
        }
    }

    // Extract the first line (up to \n)
    auto pos = data.find('\n');
    if (pos == std::string::npos) {
        spdlog::warn("LlmSqlProxy: request too large or missing newline "
                     "(received {} bytes)", data.size());
        nlohmann::json err;
        err["resp"] = kRespInvalidJson;
        err["rows"] = nlohmann::json::array();
        err["message"] = "Request too large or missing newline";
        err["timestamp"] = CurrentTimestamp();
        std::string resp_str = err.dump() + "\n";
        write(client_fd, resp_str.data(), resp_str.size());
        return false;
    }

    std::string line = data.substr(0, pos);

    try {
        request = nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::warn("LlmSqlProxy: failed to parse request JSON: {}", e.what());
        SendResponse(client_fd, kRespInvalidJson, nlohmann::json::array());
        return false;
    }

    return true;
}

// ============================================================================
// LlmSqlProxy — ProcessAndRespond
// ============================================================================

void LlmSqlProxy::ProcessAndRespond(int client_fd,
                                    const nlohmann::json &request)
{
    // 1. Validate cmd field
    if (!request.contains("cmd") || !request["cmd"].is_string()) {
        spdlog::warn("LlmSqlProxy: request missing required field 'cmd'");
        SendResponse(client_fd, kRespMissingField, nlohmann::json::array());
        return;
    }

    std::string cmd = request["cmd"].get<std::string>();

    // 2. Look up strategy
    LlmSqlStrategy *strategy = router_.Lookup(cmd);
    if (!strategy) {
        spdlog::warn("LlmSqlProxy: unknown cmd '{}'", cmd);
        SendResponse(client_fd, kRespUnknownCmd, nlohmann::json::array());
        return;
    }

    // 3. Validate params
    nlohmann::json params = request.value("params", nlohmann::json::object());
    if (!strategy->ValidateParams(params)) {
        spdlog::warn("LlmSqlProxy: invalid params for cmd '{}'", cmd);
        SendResponse(client_fd, kRespInvalidParams, nlohmann::json::array());
        return;
    }

    // 4. Build SQL
    std::string sql = strategy->GetSql(params);

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
                rows.push_back(RowToJson(stmt));
            }
        };
        ok = db_.ExecuteRead(sql, bind_fn, row_fn);
    }

    if (!ok) {
        spdlog::error("LlmSqlProxy: SQL execution failed for cmd '{}'", cmd);
        SendResponse(client_fd, kRespSqlError, nlohmann::json::array());
        return;
    }

    spdlog::info("LlmSqlProxy: cmd '{}' returned {} rows",
                 cmd, rows.size());
    SendResponse(client_fd, kRespOk, rows, extra);
}

// ============================================================================
// LlmSqlProxy — SendResponse
// ============================================================================

void LlmSqlProxy::SendResponse(int client_fd, int resp_code,
                               const nlohmann::json &rows,
                               const nlohmann::json &extra)
{
    nlohmann::json response;
    response["resp"] = resp_code;
    response["rows"] = rows;
    response["timestamp"] = CurrentTimestamp();

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

    std::string resp_str = response.dump() + "\n";

    // Write all bytes (handle partial writes and EINTR)
    size_t written = 0;
    while (written < resp_str.size()) {
        ssize_t n = write(client_fd, resp_str.data() + written,
                          resp_str.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::warn("LlmSqlProxy: write() failed: {}", strerror(errno));
            break;
        }
        if (n == 0) {
            // Should not happen for a stream socket, but handle gracefully.
            break;
        }
        written += static_cast<size_t>(n);
    }
}

// ============================================================================
// LlmSqlProxy — Helpers
// ============================================================================

std::string LlmSqlProxy::CurrentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    // UTC+8 (East-8 timezone, matching project convention)
    now_time_t += 8 * 3600;

    std::tm utc8_tm;
    gmtime_r(&now_time_t, &utc8_tm);

    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &utc8_tm);
    return std::string(buf);
}

nlohmann::json LlmSqlProxy::RowToJson(sqlite3_stmt *stmt)
{
    int col_count = sqlite3_column_count(stmt);
    nlohmann::json obj;

    for (int i = 0; i < col_count; ++i) {
        const char *name = sqlite3_column_name(stmt, i);
        std::string col_name = name ? name : "";

        int type = sqlite3_column_type(stmt, i);
        switch (type) {
        case SQLITE_INTEGER:
            obj[col_name] = sqlite3_column_int64(stmt, i);
            break;

        case SQLITE_FLOAT:
            obj[col_name] = sqlite3_column_double(stmt, i);
            break;

        case SQLITE_TEXT: {
            const char *text = reinterpret_cast<const char *>(
                sqlite3_column_text(stmt, i));
            int len = sqlite3_column_bytes(stmt, i);
            obj[col_name] = text ? std::string(text, static_cast<size_t>(len))
                                 : "";
            break;
        }

        case SQLITE_BLOB: {
            const void *blob = sqlite3_column_blob(stmt, i);
            int len = sqlite3_column_bytes(stmt, i);
            if (blob && len == 16) {
                // 16-byte BLOB → UUID string (e.g. dev_id, evt_id, user_id)
                obj[col_name] = util::BlobToUuid(
                    static_cast<const uint8_t *>(blob));
            } else if (blob && len > 0) {
                // Other BLOB → hex string
                static const char hex[] = "0123456789abcdef";
                std::string hex_str;
                hex_str.reserve(static_cast<size_t>(len) * 2);
                const auto *bytes = static_cast<const uint8_t *>(blob);
                for (int j = 0; j < len; ++j) {
                    hex_str += hex[(bytes[j] >> 4) & 0xF];
                    hex_str += hex[bytes[j] & 0xF];
                }
                obj[col_name] = std::move(hex_str);
            } else {
                obj[col_name] = "";
            }
            break;
        }

        case SQLITE_NULL:
        default:
            obj[col_name] = nullptr;
            break;
        }
    }

    return obj;
}

}  // namespace cortexlink
