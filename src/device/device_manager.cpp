#include "device/device_manager.h"

#include <optional>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "util/uuid_util.h"

namespace cortexlink {

// ===========================================================================
// Constructor / Destructor
// ===========================================================================

DeviceManager::DeviceManager(MqttClient *client)
    : mqtt_client_(client)
{
    // Create subscriptions but do NOT register them yet (Start() does that).
    broadcast_online_sub_ = std::make_unique<MqttSubscription>(
        "broadcast/online", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnBroadcastOnline(topic, payload);
        });

    heartbeat_sub_ = std::make_unique<MqttSubscription>(
        "device/+/heartbeat", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnHeartbeat(topic, payload);
        });

    device_resp_sub_ = std::make_unique<MqttSubscription>(
        "device/+/resp/s2m", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnDeviceResponse(topic, payload);
        });

    broadcast_config_sub_ = std::make_unique<MqttSubscription>(
        "broadcast/config", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnBroadcastConfig(topic, payload);
        });
}

DeviceManager::~DeviceManager()
{
    Stop();
}

// ===========================================================================
// Lifecycle
// ===========================================================================

bool DeviceManager::Start()
{
    if (!mqtt_client_) {
        spdlog::error("DeviceManager: MqttClient is null");
        return false;
    }

    if (!mqtt_client_->Subscribe(broadcast_online_sub_.get())) {
        spdlog::error("DeviceManager: failed to subscribe to broadcast/online");
        return false;
    }

    if (!mqtt_client_->Subscribe(heartbeat_sub_.get())) {
        spdlog::error("DeviceManager: failed to subscribe to device/+/heartbeat");
        return false;
    }

    if (!mqtt_client_->Subscribe(device_resp_sub_.get())) {
        spdlog::error("DeviceManager: failed to subscribe to device/+/resp/s2m");
        return false;
    }

    if (!mqtt_client_->Subscribe(broadcast_config_sub_.get())) {
        spdlog::error("DeviceManager: failed to subscribe to broadcast/config");
        return false;
    }

    running_ = true;
    heartbeat_thread_ = std::thread(&DeviceManager::HeartbeatCheckLoop, this);

    spdlog::info("DeviceManager: started (4 subscriptions, heartbeat check every 5s)");
    return true;
}

void DeviceManager::Stop()
{
    if (!running_) {
        return;
    }

    running_ = false;
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    // Reject all pending requests so no caller blocks indefinitely.
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto &kv : pending_requests_) {
            try {
                kv.second->promise.set_value("");
            } catch (const std::future_error &) {
                // Promise was already satisfied (late reply).
            }
        }
        pending_requests_.clear();
    }

    spdlog::info("DeviceManager: stopped");
}

// ===========================================================================
// Device communication
// ===========================================================================

bool DeviceManager::SendWithReply(const std::string &dev_uuid,
                                  const std::string &topic_suffix,
                                  const std::string &payload,
                                  std::string &reply_out,
                                  int timeout_ms)
{
    if (!mqtt_client_) {
        spdlog::error("DeviceManager: MqttClient is null");
        return false;
    }

    // 1. Generate a unique msg_id
    std::string msg_id = util::GenerateUuid();

    // 2. Inject msg_id into the payload JSON
    nlohmann::json msg;
    try {
        msg = payload.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::error("DeviceManager: failed to parse payload JSON: {}", e.what());
        return false;
    }
    msg["msg_id"] = msg_id;
    std::string enriched = msg.dump();

    // 3. Register a promise for this msg_id
    auto request = std::make_shared<PendingRequest>();
    request->send_time = std::chrono::steady_clock::now();
    std::future<std::string> future = request->promise.get_future();

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[msg_id] = std::move(request);
    }

    // 4. Publish
    std::string topic = "device/" + dev_uuid + "/" + topic_suffix;
    if (!mqtt_client_->PublishMessage(topic, enriched, /*qos=*/1)) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(msg_id);
        spdlog::error("DeviceManager: publish failed for topic={}", topic);
        return false;
    }

    spdlog::debug("DeviceManager: SendWithReply msg_id={} topic={}", msg_id, topic);

    // 5. Wait for reply with timeout
    auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::ready) {
        reply_out = future.get();
        // Clean up the consumed promise
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(msg_id);
        }
        spdlog::debug("DeviceManager: reply received for msg_id={}", msg_id);
        return true;
    }

    // Timeout
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_requests_.find(msg_id);
        if (it != pending_requests_.end()) {
            // If a late reply already satisfied the promise, don't double-set.
            try {
                it->second->promise.set_value("");
            } catch (const std::future_error &) {
                // Late reply beat us to it.
            }
            pending_requests_.erase(it);
        }
    }

    spdlog::error("DeviceManager: no reply from {} within {}ms (msg_id={}, topic={})",
                  dev_uuid, timeout_ms, msg_id, topic);
    return false;
}

bool DeviceManager::Send(const std::string &topic, const std::string &payload)
{
    if (!mqtt_client_) {
        spdlog::error("DeviceManager: MqttClient is null");
        return false;
    }

    std::string msg_id = util::GenerateUuid();

    nlohmann::json msg;
    try {
        msg = payload.empty() ? nlohmann::json::object()
                              : nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::error("DeviceManager: failed to parse payload JSON: {}", e.what());
        return false;
    }
    msg["msg_id"] = msg_id;

    return mqtt_client_->PublishMessage(topic, msg.dump());
}

void DeviceManager::SendM2sReply(const std::string &dev_uuid,
                                  const std::string &msg_id,
                                  DeviceRespCode resp_code)
{
    nlohmann::json reply;
    reply["msg_id"] = msg_id;
    reply["resp"] = static_cast<int>(resp_code);

    std::string topic = "device/" + dev_uuid + "/resp/m2s";
    if (!mqtt_client_->PublishMessage(topic, reply.dump(), /*qos=*/1)) {
        spdlog::error("DeviceManager: failed to send m2s reply to {} (resp={})",
                      dev_uuid, static_cast<int>(resp_code));
        return;
    }

    spdlog::debug("DeviceManager: sent m2s reply resp={} to device {}",
                  static_cast<int>(resp_code), dev_uuid);
}

// ===========================================================================
// Diagnostics
// ===========================================================================

bool DeviceManager::IsDeviceOnline(const std::string &dev_uuid)
{
    auto blob = util::UuidToBlob(dev_uuid);
    auto dev = device_table_.GetByDevId(blob);
    return dev.has_value() && dev->dev_state == "online";
}

std::chrono::steady_clock::time_point DeviceManager::GetLastHeartbeat(
    const std::array<uint8_t, 16> &dev_id) const
{
    std::string uuid_str = util::BlobToUuid(dev_id);
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    auto it = last_heartbeat_.find(uuid_str);
    if (it != last_heartbeat_.end()) {
        return it->second;
    }
    return std::chrono::steady_clock::time_point{};
}

// ===========================================================================
// MQTT subscription callbacks
// ===========================================================================

void DeviceManager::OnBroadcastOnline(const std::string & /*topic*/,
                                      const std::string &payload)
{
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::error("DeviceManager: broadcast/online JSON parse error: {}", e.what());
        return;
    }

    std::string dev_id_str = j.value("dev_id", "");
    if (dev_id_str.empty()) {
        spdlog::error("DeviceManager: broadcast/online missing dev_id");
        return;
    }

    auto dev_id_blob = util::UuidToBlob(dev_id_str);

    // Build DeviceProperty from broadcast/online payload
    DevicePropertyTable::DeviceProperty dev;
    dev.dev_id = dev_id_blob;
    dev.dev_name = j.value("dev_name", "");
    dev.dev_type = j.value("dev_type", "");
    dev.dev_subtype = j.value("dev_subtype", "");
    dev.dev_state = "online";
    dev.location = j.value("location", "");
    dev.user_param = j.value("user_param", "");

    // Convert actions array to DB-expected format: {"actions": [...]}
    if (j.contains("actions") && j["actions"].is_array()) {
        nlohmann::json wrapper;
        wrapper["actions"] = j["actions"];
        dev.actions = wrapper.dump();
    } else {
        dev.actions = R"({"actions":[]})";
    }

    // Convert event array to DB-expected format: {"evt_id": [...]}
    if (j.contains("event") && j["event"].is_array()) {
        nlohmann::json evt_ids = nlohmann::json::array();
        for (const auto &e : j["event"]) {
            if (e.contains("evt_id")) {
                evt_ids.push_back(e["evt_id"]);
            }
        }
        nlohmann::json wrapper;
        wrapper["evt_id"] = evt_ids;
        dev.events = wrapper.dump();
    } else {
        dev.events = R"({"evt_id":[]})";
    }

    // Convert data array to DB-expected format: {"data": [...]}
    if (j.contains("data") && j["data"].is_array()) {
        nlohmann::json wrapper;
        wrapper["data"] = j["data"];
        dev.data = wrapper.dump();
    } else {
        dev.data = R"({"data":[]})";
    }

    if (device_table_.Upsert(dev)) {
        spdlog::info("DeviceManager: device online — {} ({})", dev_id_str, dev.dev_name);
        SendM2sReply(dev_id_str, "", DeviceRespCode::OK);
    } else {
        spdlog::error("DeviceManager: failed to upsert device {}", dev_id_str);
        SendM2sReply(dev_id_str, "", DeviceRespCode::INTERNAL_ERROR);
        return;
    }

    // Upsert event definitions into the event table
    if (j.contains("event") && j["event"].is_array()) {
        for (const auto &e : j["event"]) {
            if (!e.contains("evt_id") || !e["evt_id"].is_string()) continue;
            if (!e.contains("evt_name") || !e["evt_name"].is_string()) continue;

            EventTable::Event evt;
            evt.evt_id   = util::UuidToBlob(e["evt_id"].get<std::string>());
            evt.dev_id   = dev_id_blob;
            evt.evt_name = e["evt_name"].get<std::string>();
            evt.desc     = e.value("desc", "");

            // Serialize params: wrap the params array in {"params": [...]}
            if (e.contains("params") && e["params"].is_array()) {
                nlohmann::json params_wrapper;
                params_wrapper["params"] = e["params"];
                evt.params = params_wrapper.dump();
            } else {
                evt.params = R"({"params":[]})";
            }

            if (!event_table_.Upsert(evt)) {
                spdlog::warn("DeviceManager: failed to upsert event {} for device {}",
                             e["evt_id"].get<std::string>(), dev_id_str);
            }
        }
    }

    // Initialize heartbeat timestamp
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        last_heartbeat_[dev_id_str] = std::chrono::steady_clock::now();
    }
}

void DeviceManager::OnHeartbeat(const std::string &topic,
                                const std::string & /*payload*/)
{
    std::string dev_uuid = ExtractDevUuid(topic);
    if (dev_uuid.empty()) {
        spdlog::warn("DeviceManager: failed to extract UUID from heartbeat topic: {}",
                     topic);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        last_heartbeat_[dev_uuid] = std::chrono::steady_clock::now();
    }

    spdlog::debug("DeviceManager: heartbeat from device {}", dev_uuid);

    // If the device is known but marked offline in the DB, bring it back online.
    auto blob = util::UuidToBlob(dev_uuid);
    auto dev = device_table_.GetByDevId(blob);
    if (dev.has_value() && dev->dev_state != "online") {
        device_table_.UpdateState(blob, "online");
        spdlog::info("DeviceManager: device {} state restored to online via heartbeat",
                     dev_uuid);
    }
}

void DeviceManager::OnDeviceResponse(const std::string &topic,
                                     const std::string &payload)
{
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error &) {
        // Not valid JSON — ignore.
        return;
    }

    std::string msg_id = j.value("msg_id", "");
    if (msg_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_requests_.find(msg_id);
    if (it != pending_requests_.end()) {
        try {
            it->second->promise.set_value(payload);
        } catch (const std::future_error &) {
            // Already satisfied (should not normally happen).
        }
        pending_requests_.erase(it);
        spdlog::debug("DeviceManager: reply matched for msg_id={}", msg_id);
    }
}

void DeviceManager::OnBroadcastConfig(const std::string & /*topic*/,
                                       const std::string &payload)
{
    // 1. Parse JSON
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::error("DeviceManager: broadcast/config JSON parse error: {}", e.what());
        return;
    }

    // 2. Extract dev_id (required)
    std::string dev_id_str = j.value("dev_id", "");
    if (dev_id_str.empty()) {
        spdlog::error("DeviceManager: broadcast/config missing dev_id");
        return;
    }

    // 3. Check device exists in DB
    auto dev_id_blob = util::UuidToBlob(dev_id_str);
    auto existing = device_table_.GetByDevId(dev_id_blob);
    if (!existing.has_value()) {
        spdlog::error("DeviceManager: broadcast/config device not found: {}",
                      dev_id_str);
        return;
    }

    // 4. Extract optional config fields
    std::optional<std::string> dev_name;
    std::optional<std::string> location;
    std::optional<std::string> user_param;

    if (j.contains("dev_name") && !j["dev_name"].is_null()) {
        dev_name = j["dev_name"].get<std::string>();
    }
    if (j.contains("location") && !j["location"].is_null()) {
        location = j["location"].get<std::string>();
    }
    if (j.contains("user_param") && !j["user_param"].is_null()) {
        user_param = j["user_param"].get<std::string>();
    }

    // At least one config field must be present
    if (!dev_name.has_value() && !location.has_value() && !user_param.has_value()) {
        spdlog::warn("DeviceManager: broadcast/config no config fields for dev={}",
                     dev_id_str);
        return;
    }

    // 5. Partial update in database
    if (!device_table_.UpdateConfigFields(dev_id_blob,
                                          dev_name, location, user_param)) {
        spdlog::error("DeviceManager: DB update failed for device {}", dev_id_str);
        return;
    }

    spdlog::info("DeviceManager: config updated for device {}", dev_id_str);

    // 6. Forward config to the device via device/{uuid}/config
    nlohmann::json config_msg;
    if (dev_name.has_value())   config_msg["dev_name"]   = *dev_name;
    if (location.has_value())   config_msg["location"]   = *location;
    if (user_param.has_value()) config_msg["user_param"] = *user_param;

    std::string config_topic = "device/" + dev_id_str + "/config";
    if (!Send(config_topic, config_msg.dump())) {
        spdlog::warn("DeviceManager: failed to forward config to {}",
                     config_topic);
    }

    // 7. Notify APP to reload device properties
    nlohmann::json reload_msg;
    reload_msg["act_id"] = "reload_device";
    reload_msg["params"] = nlohmann::json::object();

    std::string reload_topic = "device/" + dev_id_str + "/action/reload_device";
    if (!Send(reload_topic, reload_msg.dump())) {
        spdlog::warn("DeviceManager: failed to send reload_device action to {}",
                     reload_topic);
    }
}

// ===========================================================================
// Heartbeat check loop (runs on dedicated thread)
// ===========================================================================

void DeviceManager::HeartbeatCheckLoop()
{
    spdlog::info("DeviceManager: heartbeat check loop started");

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        if (!running_) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> stale_devices;

        // Collect devices whose heartbeat has timed out (> 30 s).
        {
            std::lock_guard<std::mutex> lock(heartbeat_mutex_);
            for (auto it = last_heartbeat_.begin(); it != last_heartbeat_.end();) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - it->second).count();
                if (elapsed > 30) {
                    stale_devices.push_back(it->first);
                    it = last_heartbeat_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Mark stale devices offline in the database.
        for (const auto &uuid_str : stale_devices) {
            auto blob = util::UuidToBlob(uuid_str);
            auto dev = device_table_.GetByDevId(blob);
            if (dev.has_value() && dev->dev_state == "online") {
                spdlog::warn("DeviceManager: device {} heartbeat timeout, "
                             "marking offline", uuid_str);
                device_table_.UpdateState(blob, "offline");
            }
        }

        // Clean up stale pending requests (> 5 s old — should have been
        // cleaned by SendWithReply timeout already; this is a safety net).
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.begin();
            while (it != pending_requests_.end()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->second->send_time).count();
                if (elapsed > 5000) {
                    spdlog::warn("DeviceManager: removing stale pending request "
                                 "msg_id={}", it->first);
                    try {
                        it->second->promise.set_value("");
                    } catch (const std::future_error &) {
                    }
                    it = pending_requests_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    spdlog::info("DeviceManager: heartbeat check loop ended");
}

// ===========================================================================
// Helpers
// ===========================================================================

std::string DeviceManager::ExtractDevUuid(const std::string &topic)
{
    // Expected format: "device/<uuid>/<suffix>"
    const std::string prefix = "device/";
    if (topic.compare(0, prefix.size(), prefix) != 0) {
        return {};
    }

    size_t start = prefix.size();
    size_t end = topic.find('/', start);
    if (end == std::string::npos) {
        return {};
    }

    return topic.substr(start, end - start);
}

}  // namespace cortexlink
