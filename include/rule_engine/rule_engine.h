#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

#include "db/device_data_table.h"
#include "db/device_property_table.h"
#include "db/event_record_table.h"
#include "db/event_rule_table.h"
#include "db/event_table.h"
#include "db/rule_table.h"
#include "lua/lua_sandbox.h"
#include "mqtt/mqtt_client.h"

namespace cortexlink {

// Forward declaration
class DeviceManager;

// RuleEngine is the central orchestrator that bridges the condition parser
// and the Lua sandbox. It subscribes to MQTT device-event topics, looks up
// matching rules, evaluates conditions, and executes Lua script actions.
//
// Thread model: MQTT callbacks fire on the mosquitto loop thread and
// enqueue events into a thread-safe queue. A dedicated worker thread
// dequeues and processes events serially so that Lua execution (which may
// take several seconds with retries) never blocks the MQTT loop.
class RuleEngine {
public:
    RuleEngine(MqttClient *mqtt_client, DeviceManager *device_manager);
    ~RuleEngine();

    RuleEngine(const RuleEngine &) = delete;
    RuleEngine &operator=(const RuleEngine &) = delete;

    // ---- lifecycle -------------------------------------------------------

    // Subscribe to device/+/event/# and start the background worker thread.
    // Returns false if the MQTT subscription fails.
    bool Start();

    // Signal the worker thread to stop and clean up resources.
    // Idempotent — safe to call multiple times.
    void Stop();

    // ---- internal event injection ----------------------------------------

    // Entry point for internal event sources (e.g. Cron virtual device).
    // The event is enqueued and processed by the worker thread just like
    // an MQTT-delivered event.
    void InjectEvent(const std::array<uint8_t, 16> &evt_id,
                     const std::array<uint8_t, 16> &dev_id,
                     const std::string &evt_name,
                     const std::string &params_json);

private:
    // ---- MQTT callback (fires on mosquitto loop thread) ------------------

    void OnDeviceEvent(const std::string &topic, const std::string &payload);

    // ---- internal event representation -----------------------------------

    struct EventTask {
        std::array<uint8_t, 16> evt_id;
        std::array<uint8_t, 16> dev_id;
        std::string evt_name;      // may be empty if coming from MQTT
        std::string params_json;   // raw JSON from the payload
    };

    // ---- worker thread --------------------------------------------------

    void WorkerLoop();

    // Process a single event: look up event definition, record it,
    // find matching rules, evaluate conditions, execute Lua actions.
    void ProcessEvent(const EventTask &task);

    // ---- helpers --------------------------------------------------------

    // Extract the device UUID string from a topic of the form
    // "device/<uuid>/event/<type>".
    static std::string ExtractDevUuid(const std::string &topic);

    // Convert a params JSON array of the form
    //   [{"p_name":"x","value":35.5}, ...]
    // to an unordered_map<string, string> suitable for both EvalContext
    // and LuaSandbox::EventContext.
    static std::unordered_map<std::string, std::string>
    ParseEventParams(const std::string &params_json);

    // Fill hour/min/sec with the current wall-clock time (UTC+8).
    static void GetCurrentTime(int &hour, int &min, int &sec);

    // ---- owned DB tables ------------------------------------------------

    EventTable event_table_;
    RuleTable rule_table_;
    EventRuleTable event_rule_table_;
    EventRecordTable event_record_table_;
    DeviceDataTable device_data_table_;
    DevicePropertyTable device_property_table_;

    // ---- dependencies (not owned, must outlive *this) -------------------

    MqttClient *mqtt_client_;
    DeviceManager *device_manager_;

    // ---- Lua sandbox ----------------------------------------------------

    LuaSandbox lua_sandbox_;

    // ---- MQTT subscription ----------------------------------------------

    std::unique_ptr<MqttSubscription> event_sub_;

    // ---- worker thread and event queue ----------------------------------

    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<EventTask> event_queue_;
};

}  // namespace cortexlink
