#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "db/device_property_table.h"
#include "db/event_table.h"
#include "device/device_resp_code.h"
#include "mqtt/mqtt_client.h"

namespace cortexlink {

// Forward declaration
class RuleEngine;

// CronScheduler implements the Cron virtual device (UUID 00000000-0000-0000-0000-000000000001).
//
// It subscribes to the cron device's MQTT action topic and handles add_cron,
// add_relative_cron, remove_cron, and list_crons. A background scheduler thread
// wakes every 30 seconds, evaluates cron expressions against the current UTC+8
// wall-clock time, and injects cron_trigger events into the RuleEngine when
// a job matches.
//
// Cron jobs are persisted in ~/.cortexlink/cron/crontab.txt (pipe-delimited).
//
// Thread model:
//   - MQTT callbacks fire on the mosquitto loop thread and directly modify
//     the in-memory entry list (protected by mutex_).
//   - The scheduler thread reads the entry list under the same mutex.
//   - File I/O is done under mutex_ and is lightweight (< 100 entries).
class CronScheduler {
public:
    CronScheduler(MqttClient *mqtt_client, RuleEngine *rule_engine);
    ~CronScheduler();

    CronScheduler(const CronScheduler &) = delete;
    CronScheduler &operator=(const CronScheduler &) = delete;

    // ---- lifecycle -------------------------------------------------------

    // Register the cron device and cron_trigger event in the database,
    // subscribe to the action topic, load existing jobs from crontab.txt,
    // and start the scheduler thread.
    // Returns false if any registration or subscription fails.
    bool Start();

    // Signal the scheduler thread to stop, join it, and unsubscribe.
    // Idempotent — safe to call multiple times.
    void Stop();

private:
    // ---- in-memory cron entry -------------------------------------------

    struct CronEntry {
        std::string id;            // UUID, used as cron_id
        std::string expr;          // 5-field cron expression
        std::string params;        // custom params as JSON object string ("{}" if empty)
        int trigger_count;         // -1 = infinite, >0 = remaining
    };

    // ---- MQTT callback (fires on mosquitto loop thread) -----------------

    void OnAction(const std::string &topic, const std::string &payload);

    // ---- action handlers ------------------------------------------------

    void HandleAddCron(const std::string &msg_id, const nlohmann::json &action_params);
    void HandleAddRelativeCron(const std::string &msg_id, const nlohmann::json &action_params);
    void HandleRemoveCron(const std::string &msg_id, const nlohmann::json &action_params);
    void HandleListCrons(const std::string &msg_id);

    // ---- scheduler thread -----------------------------------------------

    void SchedulerLoop();

    // ---- crontab.txt persistence ----------------------------------------

    // Parse crontab.txt into entries_. Must hold mutex_.
    bool LoadCrontab();

    // Write entries_ to crontab.txt. Must hold mutex_.
    bool SaveCrontab();

    // ---- DB registration helpers ----------------------------------------

    void RegisterCronDevice();
    void RegisterCronEvent();

    // ---- S2M response (device → host) --------------------------------

    void SendS2mResponse(const std::string &msg_id, DeviceRespCode code,
                         const std::string &data_json = "");

    // ---- fixed UUIDs ----------------------------------------------------

    static constexpr const char *kCronDevUuidStr = "00000000-0000-0000-0000-000000000001";
    static constexpr const char *kCronEvtUuidStr = "00000000-0000-0000-0000-000000000002";

    // ---- dependencies (not owned, must outlive *this) -------------------

    MqttClient *mqtt_client_;
    RuleEngine *rule_engine_;

    // ---- owned DB tables ------------------------------------------------

    DevicePropertyTable device_property_table_;
    EventTable event_table_;

    // ---- MQTT subscription ----------------------------------------------

    std::unique_ptr<MqttSubscription> action_sub_;

    // ---- scheduler thread -----------------------------------------------

    std::thread scheduler_thread_;
    std::atomic<bool> running_{false};
    std::condition_variable cv_;
    mutable std::mutex mutex_;

    // ---- cron entries (protected by mutex_) -----------------------------

    std::vector<CronEntry> entries_;

    // ---- dedup: cron_id → unix_minute of last fire ----------------------
    std::unordered_map<std::string, int64_t> last_fired_minute_;

    // ---- file paths -----------------------------------------------------

    std::string crontab_path_;
    std::string cron_dir_;
};

}  // namespace cortexlink
