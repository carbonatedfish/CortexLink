#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "mqtt/mqtt_client.h"

namespace cortexlink {

class AppFileTransManager {
public:
    explicit AppFileTransManager(MqttClient *client);
    ~AppFileTransManager();

    // Non-copyable, non-movable
    AppFileTransManager(const AppFileTransManager &) = delete;
    AppFileTransManager &operator=(const AppFileTransManager &) = delete;

    // ---- lifecycle -------------------------------------------------------

    // Subscribe to MQTT topics and start the cleanup thread.
    // Returns false if any subscription fails.
    bool Start();

    // Stop the cleanup thread and clean up resources.
    // Idempotent — safe to call multiple times or if Start() was never called.
    void Stop();

private:
    enum class FileType { kFace, kVoice };

    struct FileTransferState {
        std::string file_name;
        int total_frags = 0;
        std::unordered_map<int, std::string> fragments;
        std::string checksum;
        std::chrono::steady_clock::time_point last_frag_time;
    };

    // ---- MQTT subscription callbacks (fire on MQTT background thread) ---

    void OnFaceUpload(const std::string &topic, const std::string &payload);
    void OnVoiceUpload(const std::string &topic, const std::string &payload);

    // ---- core logic -------------------------------------------------------

    void HandleFragment(const std::string &payload, FileType type);
    void TryComplete(const std::string &key, FileType type,
                     FileTransferState state);

    // ---- helpers ----------------------------------------------------------

    static std::string MakeTransferKey(const std::string &file_name,
                                       FileType type);
    static std::string MakeRespTopic(FileType type);
    static std::string SanitizeFileName(const std::string &file_name);
    std::string MakeFileDir(FileType type) const;
    void SendAck(const std::string &resp_topic, int frag_id,
                 const std::string &status);
    static int ParseJsonInt(const nlohmann::json &j, const std::string &key,
                            int default_val);

    // ---- cryptographic helpers --------------------------------------------

    static std::string Sha256Hex(const std::vector<uint8_t> &data);
    static std::vector<uint8_t> Base64Decode(const std::string &encoded);

    // ---- background thread ------------------------------------------------

    void CleanupLoop();

    // ---- dependencies (owned elsewhere, must outlive *this) -------------

    MqttClient *mqtt_client_;

    // ---- subscriptions --------------------------------------------------

    std::unique_ptr<MqttSubscription> face_trans_sub_;
    std::unique_ptr<MqttSubscription> voice_trans_sub_;

    // ---- transfer state -------------------------------------------------
    // transfer_key → FileTransferState

    mutable std::mutex transfers_mutex_;
    std::unordered_map<std::string, FileTransferState> transfers_;

    // ---- cleanup thread -------------------------------------------------

    std::thread cleanup_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace cortexlink
