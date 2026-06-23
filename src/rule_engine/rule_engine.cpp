#include "rule_engine/rule_engine.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "device/device_manager.h"
#include "rule_engine/cond_parser.h"
#include "util/uuid_util.h"

namespace cortexlink {

using json = nlohmann::json;

// ===========================================================================
// Constructor / Destructor
// ===========================================================================

RuleEngine::RuleEngine(MqttClient *mqtt_client, DeviceManager *device_manager)
    : mqtt_client_(mqtt_client)
    , device_manager_(device_manager)
    , lua_sandbox_(&device_data_table_, &device_property_table_,
                   mqtt_client_, &rule_table_)
{
    // Resolve the script directory: ~/.cortexlink/scripts/
    const char *home = std::getenv("HOME");
    if (home) {
        script_dir_ = std::string(home) + "/.cortexlink/scripts/";
    } else {
        script_dir_ = ".cortexlink/scripts/";
    }

    // Create the MQTT subscriptions but do NOT register them yet (Start() does).
    event_sub_ = std::make_unique<MqttSubscription>(
        "device/+/event/#", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnDeviceEvent(topic, payload);
        });

    data_sub_ = std::make_unique<MqttSubscription>(
        "device/+/data/+", 1,
        [this](const std::string &topic, const std::string &payload) {
            OnDeviceData(topic, payload);
        });
}

RuleEngine::~RuleEngine()
{
    Stop();
}

// ===========================================================================
// Lifecycle
// ===========================================================================

bool RuleEngine::Start()
{
    if (!mqtt_client_) {
        spdlog::error("RuleEngine: MqttClient is null");
        return false;
    }

    if (!mqtt_client_->Subscribe(event_sub_.get())) {
        spdlog::error("RuleEngine: failed to subscribe to device/+/event/#");
        return false;
    }

    if (!mqtt_client_->Subscribe(data_sub_.get())) {
        spdlog::error("RuleEngine: failed to subscribe to device/+/data/+");
        return false;
    }

    running_ = true;
    worker_thread_ = std::thread(&RuleEngine::WorkerLoop, this);

    spdlog::info("RuleEngine: started (subscribed to device/+/event/# and device/+/data/+)");
    return true;
}

void RuleEngine::Stop()
{
    if (!running_.exchange(false)) {
        return;  // already stopped or never started
    }

    // Wake up the worker thread so it can observe running_ == false.
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
    }
    queue_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    // Unsubscribe from MQTT.
    if (mqtt_client_ && event_sub_) {
        mqtt_client_->Unsubscribe(event_sub_.get());
    }
    if (mqtt_client_ && data_sub_) {
        mqtt_client_->Unsubscribe(data_sub_.get());
    }

    spdlog::info("RuleEngine: stopped");
}

// ===========================================================================
// Internal event injection
// ===========================================================================

void RuleEngine::InjectEvent(const std::array<uint8_t, 16> &evt_id,
                             const std::array<uint8_t, 16> &dev_id,
                             const std::string &evt_name,
                             const std::string &params_json)
{
    EventTask task;
    task.evt_id = evt_id;
    task.dev_id = dev_id;
    task.evt_name = evt_name;
    task.params_json = params_json;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        event_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

// ===========================================================================
// MQTT callback (mosquitto loop thread)
// ===========================================================================

void RuleEngine::OnDeviceEvent(const std::string &topic,
                               const std::string &payload)
{
    // Parse the JSON payload.
    json j;
    try {
        j = json::parse(payload);
    } catch (const json::parse_error &e) {
        spdlog::error("RuleEngine: failed to parse event payload: {}", e.what());
        return;
    }

    // Extract evt_id string.
    std::string evt_id_str;
    if (j.contains("evt_id") && j["evt_id"].is_string()) {
        evt_id_str = j["evt_id"].get<std::string>();
    }
    if (evt_id_str.empty()) {
        spdlog::warn("RuleEngine: event payload missing evt_id, topic={}", topic);
        return;
    }

    // Extract device UUID from the topic.
    std::string dev_uuid_str = ExtractDevUuid(topic);
    if (dev_uuid_str.empty()) {
        spdlog::warn("RuleEngine: failed to extract dev UUID from topic: {}",
                     topic);
        return;
    }

    // Convert UUID strings to BLOBs.
    std::array<uint8_t, 16> evt_id_blob = util::UuidToBlob(evt_id_str);
    std::array<uint8_t, 16> dev_id_blob = util::UuidToBlob(dev_uuid_str);

    // Extract the raw params JSON.
    std::string params_json;
    if (j.contains("params")) {
        params_json = j["params"].dump();
    } else {
        params_json = "[]";
    }

    // Enqueue the task.
    EventTask task;
    task.evt_id = evt_id_blob;
    task.dev_id = dev_id_blob;
    task.evt_name.clear();  // will be filled in by ProcessEvent from DB
    task.params_json = std::move(params_json);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        event_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

// ===========================================================================
// MQTT data callback (mosquitto loop thread)
// ===========================================================================

void RuleEngine::OnDeviceData(const std::string &topic,
                              const std::string &payload)
{
    // Parse the JSON payload.
    json j;
    try {
        j = json::parse(payload);
    } catch (const json::parse_error &e) {
        spdlog::error("RuleEngine: failed to parse data payload: {}", e.what());
        return;
    }

    // Extract data_name.
    std::string data_name;
    if (j.contains("data_name") && j["data_name"].is_string()) {
        data_name = j["data_name"].get<std::string>();
    }
    if (data_name.empty()) {
        spdlog::warn("RuleEngine: data payload missing data_name, topic={}", topic);
        return;
    }

    // Extract data_val.
    std::string data_val;
    if (j.contains("data_val")) {
        if (j["data_val"].is_string()) {
            data_val = j["data_val"].get<std::string>();
        } else {
            data_val = j["data_val"].dump();
        }
    }

    // Extract device UUID from the topic.
    std::string dev_uuid_str = ExtractDevUuid(topic);
    if (dev_uuid_str.empty()) {
        spdlog::warn("RuleEngine: failed to extract dev UUID from data topic: {}",
                     topic);
        return;
    }

    std::array<uint8_t, 16> dev_id_blob = util::UuidToBlob(dev_uuid_str);

    // Try to infer data_type from the device's metadata.
    std::string data_type = "str";
    auto dev_opt = device_property_table_.GetByDevId(dev_id_blob);
    if (dev_opt.has_value() && !dev_opt->data.empty()) {
        try {
            json data_meta = json::parse(dev_opt->data);
            if (data_meta.contains("data") && data_meta["data"].is_array()) {
                for (const auto &d : data_meta["data"]) {
                    if (d.contains("d_name") && d["d_name"].is_string()
                        && d["d_name"].get<std::string>() == data_name
                        && d.contains("type") && d["type"].is_string()) {
                        data_type = d["type"].get<std::string>();
                        break;
                    }
                }
            }
        } catch (const json::parse_error &) {
            // Fall through with default "str".
        }
    }

    // Upsert into device_data table.
    DeviceDataTable::DeviceData data;
    data.dev_id = dev_id_blob;
    data.data_name = data_name;
    data.data_type = data_type;
    data.data_val = data_val;

    if (device_data_table_.Upsert(data)) {
        spdlog::debug("RuleEngine: data upserted dev={} name={} val={} type={}",
                      dev_uuid_str, data_name, data_val, data_type);
    } else {
        spdlog::error("RuleEngine: failed to upsert data dev={} name={}",
                      dev_uuid_str, data_name);
    }
}

// ===========================================================================
// Worker thread
// ===========================================================================

void RuleEngine::WorkerLoop()
{
    spdlog::info("RuleEngine: worker thread started");

    while (running_) {
        EventTask task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !running_ || !event_queue_.empty();
            });

            if (!running_ && event_queue_.empty()) {
                break;
            }

            task = std::move(event_queue_.front());
            event_queue_.pop();
        }

        ProcessEvent(task);
    }

    spdlog::info("RuleEngine: worker thread exiting");
}

// ===========================================================================
// Core event processing
// ===========================================================================

void RuleEngine::ProcessEvent(const EventTask &task)
{
    // 1. Look up the event definition.
    auto evt_opt = event_table_.GetByEvtId(task.evt_id);
    if (!evt_opt.has_value()) {
        spdlog::warn("RuleEngine: unknown event id={}, recording and skipping",
                     util::BlobToUuid(task.evt_id));
        // Still record the unknown event for audit purposes.
        EventRecordTable::EventRecord record;
        record.evt_id = task.evt_id;
        record.dev_id = task.dev_id;
        record.evt_name = task.evt_name.empty() ? "unknown" : task.evt_name;
        record.params = task.params_json;
        event_record_table_.Insert(record);
        return;
    }

    const auto &evt_def = evt_opt.value();

    // 2. Record the event in history.
    EventRecordTable::EventRecord record;
    record.evt_id = task.evt_id;
    record.dev_id = task.dev_id;
    record.evt_name = evt_def.evt_name;
    record.params = task.params_json;
    event_record_table_.Insert(record);

    // 3. Find matching rules for this event.
    auto rule_ids = event_rule_table_.GetRulesByEvtId(task.evt_id);
    if (rule_ids.empty()) {
        return;  // no rules bound to this event
    }

    std::string evt_id_str = util::BlobToUuid(task.evt_id);
    std::string dev_id_str = util::BlobToUuid(task.dev_id);

    // 4. Process each rule.
    for (int64_t rule_id : rule_ids) {
        auto rule_opt = rule_table_.GetByRuleId(rule_id);
        if (!rule_opt.has_value()) {
            spdlog::error("RuleEngine: rule {} not found in DB (listed in event_rule)",
                          rule_id);
            continue;
        }

        const auto &rule = rule_opt.value();

        // 4a. Check if rule is enabled.
        if (!rule.enable) {
            spdlog::debug("RuleEngine: rule {} ({}) is disabled, skipping",
                          rule_id, rule.rule_name);
            continue;
        }

        // 4b. Check rate limit (limit == 0 means unlimited).
        if (rule.limit > 0 && rule.count >= rule.limit) {
            spdlog::debug("RuleEngine: rule {} ({}) reached limit {}/{}, skipping",
                          rule_id, rule.rule_name, rule.count, rule.limit);
            continue;
        }

        // 4c. Evaluate condition expression (if present).
        if (!rule.cond_expr.empty()) {
            auto ast = ParseCondition(rule.cond_expr, evt_id_str);
            if (ast != nullptr) {
                // Parse event params for condition evaluation.
                auto event_params = ParseEventParams(task.params_json);

                // Build evaluation context.
                EvalContext ctx;
                ctx.event_params = std::move(event_params);
                ctx.get_device_data =
                    [this](const std::string &dev_id,
                           const std::string &data_name)
                    -> std::optional<std::string> {
                    auto blob = util::UuidToBlob(dev_id);
                    auto result = device_data_table_.Get(blob, data_name);
                    if (result.has_value()) {
                        return result->data_val;
                    }
                    return std::nullopt;
                };
                GetCurrentTime(ctx.hour, ctx.min, ctx.sec);

                bool cond_result = EvaluateAst(ast.get(), ctx);
                if (!cond_result) {
                    spdlog::debug("RuleEngine: rule {} ({}) condition false, skipping",
                                  rule_id, rule.rule_name);
                    continue;
                }
            }
            // If ParseCondition returns nullptr, the condition is effectively
            // empty for this event (e.g. "evt_id()" — always-true). Proceed.
        }

        // 4d. Read the Lua script from disk.
        std::string script_path = script_dir_ + rule.action;
        std::ifstream script_file(script_path);
        if (!script_file.is_open()) {
            spdlog::error("RuleEngine: rule {} ({}) script file not found: {}",
                          rule_id, rule.rule_name, script_path);
            continue;
        }

        std::string script_text((std::istreambuf_iterator<char>(script_file)),
                                std::istreambuf_iterator<char>());
        script_file.close();

        if (script_text.empty()) {
            spdlog::error("RuleEngine: rule {} ({}) script file is empty: {}",
                          rule_id, rule.rule_name, script_path);
            continue;
        }

        // 4e. Build LuaSandbox event context and execute the action.
        LuaSandbox::EventContext lua_ctx;
        lua_ctx.evt_id = evt_id_str;
        lua_ctx.evt_name = evt_def.evt_name;
        lua_ctx.dev_id = dev_id_str;
        lua_ctx.params = ParseEventParams(task.params_json);

        spdlog::info("RuleEngine: executing rule {} ({}) for event {}",
                     rule_id, rule.rule_name, evt_def.evt_name);

        bool success = lua_sandbox_.Execute(script_text, lua_ctx, rule_id);
        if (success) {
            spdlog::info("RuleEngine: rule {} ({}) executed successfully",
                         rule_id, rule.rule_name);
        } else {
            spdlog::warn("RuleEngine: rule {} ({}) execution failed",
                         rule_id, rule.rule_name);
        }
    }
}

// ===========================================================================
// Helpers
// ===========================================================================

std::string RuleEngine::ExtractDevUuid(const std::string &topic)
{
    // Expected format: "device/<uuid>/event/<type>"
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

std::unordered_map<std::string, std::string>
RuleEngine::ParseEventParams(const std::string &params_json)
{
    std::unordered_map<std::string, std::string> result;

    if (params_json.empty()) {
        return result;
    }

    try {
        json params = json::parse(params_json);
        if (!params.is_array()) {
            return result;
        }

        for (const auto &p : params) {
            if (!p.is_object()) {
                continue;
            }
            if (!p.contains("p_name") || !p["p_name"].is_string()) {
                continue;
            }

            std::string name = p["p_name"].get<std::string>();

            if (!p.contains("value")) {
                continue;
            }

            // Convert the value to its string representation.
            std::string val_str;
            if (p["value"].is_string()) {
                val_str = p["value"].get<std::string>();
            } else if (p["value"].is_number()) {
                val_str = p["value"].dump();
            } else if (p["value"].is_boolean()) {
                val_str = p["value"].get<bool>() ? "true" : "false";
            } else {
                val_str = p["value"].dump();
            }

            result[name] = val_str;
        }
    } catch (const json::parse_error &e) {
        spdlog::warn("RuleEngine: failed to parse event params JSON: {}", e.what());
    }

    return result;
}

void RuleEngine::GetCurrentTime(int &hour, int &min, int &sec)
{
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    // Convert to UTC+8 (east-8 timezone).
    now_time_t += 8 * 3600;

    std::tm utc8_tm;
    gmtime_r(&now_time_t, &utc8_tm);

    hour = utc8_tm.tm_hour;
    min = utc8_tm.tm_min;
    sec = utc8_tm.tm_sec;
}

}  // namespace cortexlink
