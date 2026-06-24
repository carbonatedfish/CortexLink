#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "db/device_property_table.h"
#include "device/device_resp_code.h"
#include "mqtt/mqtt_client.h"

namespace cortexlink {

class DeviceManager {
public:
    explicit DeviceManager(MqttClient *client);
    ~DeviceManager();

    // Non-copyable, non-movable
    DeviceManager(const DeviceManager &) = delete;
    DeviceManager &operator=(const DeviceManager &) = delete;

    // ---- lifecycle -------------------------------------------------------

    // Subscribe to MQTT topics and start the heartbeat checker thread.
    // Returns false if any subscription fails.
    bool Start();

    // Stop the heartbeat checker thread and clean up resources.
    // Idempotent — safe to call multiple times or if Start() was never called.
    void Stop();

    // ---- device communication --------------------------------------------

    // Publish to device/<uuid>/<topic_suffix> and wait for a reply on
    // device/<uuid>/resp/s2m with a matching msg_id.
    //
    // A msg_id is automatically generated and injected into payload.
    // Returns true if a reply arrives within timeout_ms.
    // On timeout, logs an ERROR and returns false.
    bool SendWithReply(const std::string &dev_uuid,
                       const std::string &topic_suffix,
                       const std::string &payload,
                       std::string &reply_out,
                       int timeout_ms = 1000);

    // Fire-and-forget publish to any topic. A msg_id is auto-injected.
    bool Send(const std::string &topic, const std::string &payload);

    // Send a reply on device/<uuid>/resp/m2s.
    // Pass the request's msg_id when replying to a specific request;
    // pass an empty string for unsolicited notifications (e.g. broadcast/online).
    void SendM2sReply(const std::string &dev_uuid,
                      const std::string &msg_id,
                      DeviceRespCode resp_code);

    // ---- diagnostics -----------------------------------------------------

    // Returns true if the device is recorded as "online" in the database.
    bool IsDeviceOnline(const std::string &dev_uuid);

    // Returns the last heartbeat time for a device, or a zero time_point if
    // the device has never been heard from.
    std::chrono::steady_clock::time_point GetLastHeartbeat(
        const std::array<uint8_t, 16> &dev_id) const;

private:
    // ---- MQTT subscription callbacks (fire on MQTT background thread) ---

    void OnBroadcastOnline(const std::string &topic,
                           const std::string &payload);
    void OnHeartbeat(const std::string &topic,
                     const std::string &payload);
    void OnDeviceResponse(const std::string &topic,
                          const std::string &payload);

    // ---- background thread ----------------------------------------------

    void HeartbeatCheckLoop();

    // ---- helpers --------------------------------------------------------

    // Extract UUID string from a topic of the form "device/<uuid>/<suffix>".
    static std::string ExtractDevUuid(const std::string &topic);

    // ---- dependencies (owned elsewhere, must outlive *this) -------------

    MqttClient *mqtt_client_;
    DevicePropertyTable device_table_;

    // ---- subscriptions --------------------------------------------------

    std::unique_ptr<MqttSubscription> broadcast_online_sub_;
    std::unique_ptr<MqttSubscription> heartbeat_sub_;
    std::unique_ptr<MqttSubscription> device_resp_sub_;

    // ---- heartbeat tracking ---------------------------------------------
    // dev_uuid_string → last heartbeat timestamp (steady clock)

    mutable std::mutex heartbeat_mutex_;
    std::unordered_map<std::string,
                       std::chrono::steady_clock::time_point> last_heartbeat_;

    // ---- pending replies ------------------------------------------------
    // msg_id → promise that will be fulfilled by OnDeviceResponse

    struct PendingRequest {
        std::promise<std::string> promise;
        std::chrono::steady_clock::time_point send_time;
    };
    mutable std::mutex pending_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingRequest>> pending_requests_;

    // ---- heartbeat checker thread ---------------------------------------

    std::thread heartbeat_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace cortexlink
