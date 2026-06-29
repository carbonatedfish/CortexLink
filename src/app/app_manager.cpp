#include "app/app_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "util/uuid_util.h"

namespace cortexlink {

// ============================================================================
// Construction / Destruction
// ============================================================================

AppManager::AppManager(MqttClient *client,
                       const std::string &openclaw_endpoint)
    : mqtt_client_(client)
{
    using namespace std::placeholders;

    face_trans_sub_ = std::make_unique<MqttSubscription>(
        "app/face/trans", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnFaceUpload(topic, payload);
        });

    voice_trans_sub_ = std::make_unique<MqttSubscription>(
        "app/voice/trans", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnVoiceUpload(topic, payload);
        });

    llm_trans_sub_ = std::make_unique<MqttSubscription>(
        "app/llm/trans", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnLlmRequest(topic, payload);
        });

    open_claw_client_ = std::make_unique<OpenClawClient>();
    open_claw_client_->SetEndpoint(openclaw_endpoint);
}

AppManager::~AppManager()
{
    Stop();
}

// ============================================================================
// Lifecycle
// ============================================================================

bool AppManager::Start()
{
    if (running_) {
        spdlog::warn("AppManager::Start called while already running");
        return true;
    }

    if (!mqtt_client_->Subscribe(face_trans_sub_.get())) {
        spdlog::error("AppManager: failed to subscribe to app/face/trans");
        return false;
    }

    if (!mqtt_client_->Subscribe(voice_trans_sub_.get())) {
        spdlog::error("AppManager: failed to subscribe to app/voice/trans");
        mqtt_client_->Unsubscribe(face_trans_sub_.get());
        return false;
    }

    if (!mqtt_client_->Subscribe(llm_trans_sub_.get())) {
        spdlog::error("AppManager: failed to subscribe to app/llm/trans");
        mqtt_client_->Unsubscribe(voice_trans_sub_.get());
        mqtt_client_->Unsubscribe(face_trans_sub_.get());
        return false;
    }

    running_ = true;
    cleanup_thread_ = std::thread(&AppManager::CleanupLoop, this);
    llm_worker_thread_ = std::thread(&AppManager::LlmWorkerLoop, this);

    spdlog::info("AppManager started");
    return true;
}

void AppManager::Stop()
{
    if (!running_) return;

    running_ = false;

    llm_queue_cv_.notify_all();

    if (llm_worker_thread_.joinable()) {
        llm_worker_thread_.join();
    }

    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }

    mqtt_client_->Unsubscribe(llm_trans_sub_.get());
    mqtt_client_->Unsubscribe(voice_trans_sub_.get());
    mqtt_client_->Unsubscribe(face_trans_sub_.get());

    {
        std::lock_guard<std::mutex> lock(transfers_mutex_);
        transfers_.clear();
    }

    spdlog::info("AppManager stopped");
}

// ============================================================================
// MQTT Callbacks
// ============================================================================

void AppManager::OnFaceUpload(const std::string & /*topic*/,
                              const std::string &payload)
{
    HandleFragment(payload, FileType::kFace);
}

void AppManager::OnVoiceUpload(const std::string & /*topic*/,
                               const std::string &payload)
{
    HandleFragment(payload, FileType::kVoice);
}

void AppManager::OnLlmRequest(const std::string & /*topic*/,
                               const std::string &payload)
{
    // 1. Parse JSON
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::warn("AppManager: LLM request JSON parse error: {}", e.what());
        SendLlmResponse("", 1);
        return;
    }

    // 2. Validate required fields
    std::string msg_id = j.value("msg_id", "");
    std::string prompt = j.value("prompt", "");

    if (msg_id.empty() || prompt.empty()) {
        spdlog::warn("AppManager: LLM request missing msg_id or prompt");
        SendLlmResponse(msg_id, 2);
        return;
    }

    // 3. Enqueue for background processing
    LlmTask task;
    task.msg_id = msg_id;
    task.prompt = prompt;
    task.timestamp = j.value("timestamp", "");

    {
        std::lock_guard<std::mutex> lock(llm_queue_mutex_);
        llm_queue_.push(std::move(task));
    }
    llm_queue_cv_.notify_one();

    spdlog::info("AppManager: LLM request enqueued (msg_id={}, prompt_len={})",
                 msg_id, prompt.size());
}

// ============================================================================
// Core Logic
// ============================================================================

void AppManager::HandleFragment(const std::string &payload, FileType type)
{
    // 1. Parse JSON
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::warn("AppManager: failed to parse fragment JSON: {}", e.what());
        SendAck(MakeRespTopic(type), -1, FileRespCode::kInvalidFrag);
        return;
    }

    // 2. Extract fields
    int frag_id = ParseJsonInt(j, "frag_id", -1);
    int total_frags = ParseJsonInt(j, "total_frags", 0);
    std::string checksum = j.value("checksum", "");
    std::string file_name = j.value("file_name", "");
    std::string data = j.value("data", "");

    // 3. Validate required fields
    if (frag_id < 0 || file_name.empty() || data.empty()) {
        spdlog::warn("AppManager: invalid fragment — missing required fields "
                     "(frag_id={}, file_name='{}', data_len={})",
                     frag_id, file_name, data.size());
        SendAck(MakeRespTopic(type), frag_id >= 0 ? frag_id : -1, FileRespCode::kInvalidFrag);
        return;
    }

    // 4. Sanitize filename
    std::string safe_name = SanitizeFileName(file_name);
    if (safe_name.empty()) {
        spdlog::warn("AppManager: rejected filename '{}'", file_name);
        SendAck(MakeRespTopic(type), frag_id, FileRespCode::kInvalidFrag);
        return;
    }

    std::string key = MakeTransferKey(safe_name, type);
    std::string resp_topic = MakeRespTopic(type);
    auto now = std::chrono::steady_clock::now();

    // State extracted for completion processing outside the lock.
    FileTransferState completed_state;
    bool should_complete = false;

    {
        std::lock_guard<std::mutex> lock(transfers_mutex_);

        // 5. First fragment: create new transfer state (overwrite any existing)
        if (frag_id == 0) {
            FileTransferState state;
            state.file_name = safe_name;
            state.total_frags = total_frags;
            state.checksum = checksum;
            state.last_frag_time = now;
            state.fragments[0] = data;
            transfers_[key] = std::move(state);

            spdlog::info("AppManager: transfer started for '{}' ({} frags, key={})",
                         safe_name, total_frags, key);
            spdlog::debug("AppManager: new transfer state created key={} total_frags={} checksum={}",
                          key, total_frags, checksum);
            SendAck(resp_topic, frag_id, FileRespCode::kOk);
            return;
        }

        // 6. Not the first fragment — must have existing state
        auto it = transfers_.find(key);
        if (it == transfers_.end()) {
            spdlog::warn("AppManager: fragment {} for unknown transfer '{}'",
                         frag_id, key);
            SendAck(resp_topic, frag_id, FileRespCode::kInvalidFrag);
            return;
        }

        auto &state = it->second;

        // 7. Check fragment timeout (2 seconds)
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - state.last_frag_time);
        if (elapsed.count() > 2000) {
            spdlog::warn("AppManager: transfer '{}' timed out ({}ms since last fragment)",
                         key, elapsed.count());
            transfers_.erase(it);
            SendAck(resp_topic, frag_id, FileRespCode::kTimeout);
            return;
        }

        // 8. Update total_frags if the new fragment declares a different count
        if (total_frags != state.total_frags) {
            spdlog::info("AppManager: transfer '{}' total_frags changed {} → {}",
                         key, state.total_frags, total_frags);
            state.total_frags = total_frags;
        }

        // Keep checksum from first fragment if provided there; if the first
        // fragment omitted it, pick it up from a later fragment.
        if (state.checksum.empty() && !checksum.empty()) {
            state.checksum = checksum;
        }

        // 9. Store fragment
        state.fragments[frag_id] = data;
        state.last_frag_time = now;

        spdlog::debug("AppManager: received fragment {}/{} for '{}' (data_len={})",
                      frag_id + 1, state.total_frags, key, data.size());
        SendAck(resp_topic, frag_id, FileRespCode::kOk);

        // 10. Check if all fragments received — extract state and erase from
        //     the map so we can do I/O + SHA outside the lock.
        if (static_cast<int>(state.fragments.size()) == state.total_frags) {
            completed_state = std::move(state);
            transfers_.erase(it);
            should_complete = true;
        }
    }

    if (should_complete) {
        TryComplete(key, type, std::move(completed_state));
    }
}

void AppManager::TryComplete(const std::string &key, FileType type,
                             FileTransferState state)
{
    std::string resp_topic = MakeRespTopic(type);

    // 1. Reassemble fragments in order
    spdlog::debug("AppManager: reassembling {} fragments for '{}'",
                  state.total_frags, key);
    std::vector<uint8_t> file_data;
    for (int i = 0; i < state.total_frags; ++i) {
        auto frag_it = state.fragments.find(i);
        if (frag_it == state.fragments.end()) {
            spdlog::error("AppManager: missing fragment {} for '{}' during reassembly",
                          i, key);
            SendAck(resp_topic, state.total_frags - 1, FileRespCode::kInvalidFrag);
            return;
        }
        std::vector<uint8_t> decoded = Base64Decode(frag_it->second);
        file_data.insert(file_data.end(), decoded.begin(), decoded.end());
    }

    // 2. Validate checksum
    if (!state.checksum.empty()) {
        std::string computed = Sha256Hex(file_data);
        // Case-insensitive comparison
        std::string expected_lower = state.checksum;
        std::transform(expected_lower.begin(), expected_lower.end(),
                       expected_lower.begin(), ::tolower);
        if (computed != expected_lower) {
            spdlog::warn("AppManager: checksum mismatch for '{}' "
                         "(expected={}, computed={})",
                         key, expected_lower, computed);
            SendAck(resp_topic, state.total_frags - 1, FileRespCode::kChecksumErr);
            return;
        }
        spdlog::debug("AppManager: checksum OK for '{}'", key);
    }

    // 3. Write file to disk
    std::string dir = MakeFileDir(type);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        spdlog::error("AppManager: failed to create directory '{}': {}",
                      dir, ec.message());
        SendAck(resp_topic, state.total_frags - 1, FileRespCode::kInternalErr);
        return;
    }

    std::string file_path = dir + state.file_name;
    std::ofstream ofs(file_path, std::ios::binary);
    if (!ofs) {
        spdlog::error("AppManager: failed to open '{}' for writing", file_path);
        SendAck(resp_topic, state.total_frags - 1, FileRespCode::kInternalErr);
        return;
    }
    ofs.write(reinterpret_cast<const char *>(file_data.data()),
              static_cast<std::streamsize>(file_data.size()));
    if (!ofs) {
        spdlog::error("AppManager: failed to write '{}'", file_path);
        SendAck(resp_topic, state.total_frags - 1, FileRespCode::kInternalErr);
        return;
    }
    ofs.close();

    spdlog::info("AppManager: transfer complete — saved '{}' ({} bytes)",
                 file_path, file_data.size());
    spdlog::debug("AppManager: file written path='{}' size={}",
                  file_path, file_data.size());
    SendAck(resp_topic, state.total_frags - 1, FileRespCode::kOk);
}

// ============================================================================
// Helpers
// ============================================================================

std::string AppManager::MakeTransferKey(const std::string &file_name,
                                        FileType type)
{
    switch (type) {
    case FileType::kFace:
        return "face:" + file_name;
    case FileType::kVoice:
        return "voice:" + file_name;
    }
    return "?:" + file_name;
}

std::string AppManager::MakeRespTopic(FileType type)
{
    switch (type) {
    case FileType::kFace:
        return "app/face/resp";
    case FileType::kVoice:
        return "app/voice/resp";
    }
    return "app/?/resp";
}

std::string AppManager::SanitizeFileName(const std::string &file_name)
{
    // Extract basename: everything after the last '/' or '\'
    auto pos = file_name.find_last_of("/\\");
    std::string base = (pos == std::string::npos) ? file_name : file_name.substr(pos + 1);

    // Reject empty, ".", ".."
    if (base.empty() || base == "." || base == "..") {
        return {};
    }

    // Reject names containing null bytes
    if (base.find('\0') != std::string::npos) {
        return {};
    }

    return base;
}

std::string AppManager::MakeFileDir(FileType type) const
{
    const char *home = std::getenv("HOME");
    std::string prefix = home ? std::string(home) + "/.cortexlink" : ".cortexlink";

    switch (type) {
    case FileType::kFace:
        return prefix + "/face/";
    case FileType::kVoice:
        return prefix + "/voice/";
    }
    return prefix + "/unknown/";
}

void AppManager::SendAck(const std::string &resp_topic, int frag_id,
                         FileRespCode status)
{
    nlohmann::json resp;
    resp["frag_id"] = frag_id;
    resp["resp"] = static_cast<int>(status);
    mqtt_client_->PublishMessage(resp_topic, resp.dump(), 1);
}

int AppManager::ParseJsonInt(const nlohmann::json &j, const std::string &key,
                             int default_val)
{
    if (!j.contains(key)) return default_val;

    const auto &v = j[key];
    if (v.is_number_integer()) {
        return v.get<int>();
    }
    if (v.is_string()) {
        try {
            return std::stoi(v.get_ref<const std::string &>());
        } catch (...) {
            return default_val;
        }
    }
    return default_val;
}

// ============================================================================
// Cryptographic Helpers
// ============================================================================

std::string AppManager::Sha256Hex(const std::vector<uint8_t> &data)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        spdlog::error("AppManager: EVP_MD_CTX_new failed");
        return {};
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        spdlog::error("AppManager: EVP_DigestInit_ex failed");
        EVP_MD_CTX_free(ctx);
        return {};
    }

    if (EVP_DigestUpdate(ctx, data.data(), data.size()) != 1) {
        spdlog::error("AppManager: EVP_DigestUpdate failed");
        EVP_MD_CTX_free(ctx);
        return {};
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        spdlog::error("AppManager: EVP_DigestFinal_ex failed");
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);

    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash_len * 2);
    for (unsigned int i = 0; i < hash_len; ++i) {
        result += hex_chars[(hash[i] >> 4) & 0xF];
        result += hex_chars[hash[i] & 0xF];
    }
    return result;
}

std::vector<uint8_t> AppManager::Base64Decode(const std::string &encoded)
{
    if (encoded.empty()) return {};

    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::vector<uint8_t> result;
    result.reserve((encoded.size() * 3) / 4);

    int val = 0;
    int bits = -8;
    for (unsigned char c : encoded) {
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ') continue;

        auto pos = base64_chars.find(static_cast<char>(c));
        if (pos == std::string::npos) {
            // Invalid character — return what we have so far
            spdlog::warn("AppManager: invalid base64 character '{}' (0x{:02x})",
                         static_cast<char>(c), static_cast<int>(c));
            return {};
        }

        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            result.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return result;
}

// ============================================================================
// Cleanup Thread
// ============================================================================

void AppManager::CleanupLoop()
{
    spdlog::info("AppManager: cleanup thread started");

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();
        std::vector<std::pair<std::string, FileType>> timed_out;

        {
            std::lock_guard<std::mutex> lock(transfers_mutex_);
            for (auto it = transfers_.begin(); it != transfers_.end();) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->second.last_frag_time);
                if (elapsed.count() > 2000) {
                    // Determine FileType from the key prefix
                    FileType ft = (it->first.compare(0, 5, "face:") == 0)
                                      ? FileType::kFace
                                      : FileType::kVoice;
                    spdlog::warn("AppManager: cleanup thread aborting "
                                 "timed-out transfer '{}' ({}ms)",
                                 it->first, elapsed.count());
                    timed_out.emplace_back(it->first, ft);
                    it = transfers_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Send timeout responses outside the lock (APP may no longer be
        // listening, but we attempt delivery anyway).
        for (const auto &entry : timed_out) {
            SendAck(MakeRespTopic(entry.second), -1, FileRespCode::kTimeout);
        }
    }

    spdlog::info("AppManager: cleanup thread stopped");
}

// ============================================================================
// LLM Worker Thread
// ============================================================================

void AppManager::LlmWorkerLoop()
{
    spdlog::info("AppManager: LLM worker thread started");

    while (running_) {
        LlmTask task;

        {
            std::unique_lock<std::mutex> lock(llm_queue_mutex_);
            llm_queue_cv_.wait(lock, [this] {
                return !running_ || !llm_queue_.empty();
            });

            if (!running_ && llm_queue_.empty()) {
                break;
            }

            task = std::move(llm_queue_.front());
            llm_queue_.pop();
        }

        spdlog::debug("AppManager: LLM worker dequeued msg_id={} prompt_len={}",
                      task.msg_id, task.prompt.size());

        ProcessLlmRequest(task.msg_id, task.prompt);
    }

    spdlog::info("AppManager: LLM worker thread stopped");
}

// ============================================================================
// LLM Processing
// ============================================================================

void AppManager::ProcessLlmRequest(const std::string &msg_id,
                                    const std::string &prompt)
{
    // 1. Generate a new session ID for this request
    std::string session = util::GenerateUuid();

    spdlog::info("AppManager: processing LLM request (msg_id={}, session={})",
                 msg_id, session);

    // 2. Send prompt to OpenClaw
    auto send_resp = open_claw_client_->SendMessageAndGetResponse(session, prompt);
    if (!send_resp) {
        spdlog::error("AppManager: OpenClaw send failed for msg_id={}", msg_id);
        SendLlmResponse(msg_id, 3);
        return;
    }

    spdlog::info("AppManager: prompt sent to OpenClaw (msg_id={}, session={})",
                 msg_id, session);

    // 3. Poll for completion (max ~30s, 1s interval)
    static constexpr int kMaxPollAttempts = 30;
    static constexpr int kPollIntervalMs = 1000;

    for (int attempt = 0; attempt < kMaxPollAttempts; ++attempt) {
        if (!running_) {
            spdlog::warn("AppManager: shutting down while polling for msg_id={}",
                         msg_id);
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));

        auto history = open_claw_client_->GetHistory(session);
        if (!history) {
            spdlog::debug("AppManager: history poll failed (attempt {}/{}), "
                          "retrying", attempt + 1, kMaxPollAttempts);
            continue;
        }

        if (IsHistoryComplete(*history)) {
            spdlog::debug("AppManager: history complete msg_id={} session={}",
                          msg_id, session);
            spdlog::info("AppManager: LLM request completed (msg_id={}, "
                         "session={})", msg_id, session);
            SendLlmResponse(msg_id, 0);
            return;
        }

        spdlog::debug("AppManager: history poll not yet complete "
                      "(attempt {}/{})", attempt + 1, kMaxPollAttempts);
    }

    // 4. Polling timed out
    spdlog::warn("AppManager: LLM request timed out (msg_id={}, session={})",
                 msg_id, session);
    SendLlmResponse(msg_id, 4);
}

// ============================================================================
// History Completion Check
// ============================================================================

bool AppManager::IsHistoryComplete(const nlohmann::json &history)
{
    // Case 1: top-level "status" field with completion value
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

    // Case 3: "result" or "data" field with sub-status
    if (history.contains("result") && history["result"].is_object()) {
        return IsHistoryComplete(history["result"]);
    }

    if (history.contains("data") && history["data"].is_object()) {
        return IsHistoryComplete(history["data"]);
    }

    return false;
}

// ============================================================================
// LLM Response
// ============================================================================

void AppManager::SendLlmResponse(const std::string &msg_id, int resp_code)
{
    // Generate timestamp in UTC+8
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    now_time_t += 8 * 3600;
    std::tm utc8_tm;
    gmtime_r(&now_time_t, &utc8_tm);
    char ts_buf[64];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &utc8_tm);

    nlohmann::json resp;
    resp["msg_id"] = msg_id;
    resp["resp"] = resp_code;
    resp["timestamp"] = std::string(ts_buf);

    mqtt_client_->PublishMessage("app/llm/resp", resp.dump(), 1);

    spdlog::info("AppManager: sent app/llm/resp (msg_id={}, resp={})",
                 msg_id, resp_code);
}

}  // namespace cortexlink
