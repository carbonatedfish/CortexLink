#include "cron/cron_scheduler.h"

#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cron/cron_parser.h"
#include "rule_engine/rule_engine.h"
#include "util/uuid_util.h"

using json = nlohmann::json;

namespace cortexlink {

// ===========================================================================
// Constructor / Destructor
// ===========================================================================

CronScheduler::CronScheduler(MqttClient *mqtt_client, RuleEngine *rule_engine)
    : mqtt_client_(mqtt_client)
    , rule_engine_(rule_engine)
{
    const char *home = std::getenv("HOME");
    if (home) {
        cron_dir_ = std::string(home) + "/.cortexlink/cron";
        crontab_path_ = cron_dir_ + "/crontab.txt";
    }
}

CronScheduler::~CronScheduler()
{
    Stop();
}

// ===========================================================================
// Lifecycle
// ===========================================================================

bool CronScheduler::Start()
{
    if (running_) return true;

    // 1. Create ~/.cortexlink/cron/ directory.
    if (!cron_dir_.empty()) {
        mkdir(cron_dir_.c_str(), 0755);
    }

    // 2. Register the cron virtual device in device_property.
    RegisterCronDevice();

    // 3. Register the cron_trigger event in the event table.
    RegisterCronEvent();

    // 4. Subscribe to device/<cron_uuid>/action/#.
    std::string action_topic = std::string("device/") + kCronDevUuidStr + "/action/#";
    action_sub_ = std::make_unique<MqttSubscription>(
        action_topic, 1,
        [this](const std::string &topic, const std::string &payload) {
            OnAction(topic, payload);
        });

    if (!mqtt_client_->Subscribe(action_sub_.get())) {
        spdlog::error("CronScheduler: failed to subscribe to {}", action_topic);
        action_sub_.reset();
        return false;
    }
    spdlog::info("CronScheduler: subscribed to {}", action_topic);

    // 5. Load existing cron jobs from crontab.txt.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!LoadCrontab()) {
            spdlog::warn("CronScheduler: failed to load crontab, starting with empty list");
        }
        spdlog::info("CronScheduler: loaded {} cron job(s)", entries_.size());
    }

    // 6. Start the scheduler thread.
    running_ = true;
    scheduler_thread_ = std::thread(&CronScheduler::SchedulerLoop, this);

    spdlog::info("CronScheduler: started");
    return true;
}

void CronScheduler::Stop()
{
    if (!running_) return;

    spdlog::info("CronScheduler: stopping...");
    running_ = false;
    cv_.notify_one();

    if (scheduler_thread_.joinable()) {
        scheduler_thread_.join();
    }

    if (action_sub_) {
        mqtt_client_->Unsubscribe(action_sub_.get());
        action_sub_.reset();
    }

    spdlog::info("CronScheduler: stopped");
}

// ===========================================================================
// DB registration
// ===========================================================================

void CronScheduler::RegisterCronDevice()
{
    auto dev_id_blob = util::UuidToBlob(kCronDevUuidStr);

    DevicePropertyTable::DeviceProperty dev;
    dev.dev_id = dev_id_blob;
    dev.dev_name = "Cron Timer";
    dev.dev_type = "composite";
    dev.dev_state = "online";
    dev.location = "";
    dev.user_param = "";

    // Build actions JSON
    json actions_json;
    actions_json["actions"] = json::array({
        {
            {"act_id", "add_cron"},
            {"act_name", "Add Cron Job"},
            {"desc", "Add a cron job with a standard 5-field cron expression"},
            {"params", json::array({
                {{"p_name", "expr"}, {"desc", "5-field cron expression (min hour dom month dow)"}, {"p_type", "str"}, {"range", json::array({"", ""})}, {"unit", ""}},
                {{"p_name", "params"}, {"desc", "Custom parameters injected into cron_trigger event"}, {"p_type", "str"}, {"range", json::array({"", ""})}, {"unit", ""}},
                {{"p_name", "trigger_count"}, {"desc", "Max trigger count (-1=infinite, N=auto-delete after N)"}, {"p_type", "int"}, {"range", json::array({"", ""})}, {"unit", ""}}
            })},
            {"pre_cond", ""}
        },
        {
            {"act_id", "add_relative_cron"},
            {"act_name", "Add Relative Cron Job"},
            {"desc", "Add a one-shot cron job relative to current time"},
            {"params", json::array({
                {{"p_name", "offset"}, {"desc", "Time offset (e.g. 30m, 2h, 1d, 1h30m)"}, {"p_type", "str"}, {"range", json::array({"", ""})}, {"unit", ""}},
                {{"p_name", "params"}, {"desc", "Custom parameters injected into cron_trigger event"}, {"p_type", "str"}, {"range", json::array({"", ""})}, {"unit", ""}},
                {{"p_name", "trigger_count"}, {"desc", "Max trigger count (-1=infinite, default 1)"}, {"p_type", "int"}, {"range", json::array({"", ""})}, {"unit", ""}}
            })},
            {"pre_cond", ""}
        },
        {
            {"act_id", "remove_cron"},
            {"act_name", "Remove Cron Job"},
            {"desc", "Remove a cron job by its ID"},
            {"params", json::array({
                {{"p_name", "cron_id"}, {"desc", "Cron job UUID"}, {"p_type", "str"}, {"range", json::array({"", ""})}, {"unit", ""}}
            })},
            {"pre_cond", ""}
        },
        {
            {"act_id", "list_crons"},
            {"act_name", "List Cron Jobs"},
            {"desc", "List all cron jobs"},
            {"params", json::array()},
            {"pre_cond", ""}
        }
    });
    dev.actions = actions_json.dump();

    // Build events JSON — reference the cron_trigger event UUID
    json events_json;
    events_json["evt_id"] = json::array({kCronEvtUuidStr});
    dev.events = events_json.dump();

    // No data points for the cron device
    dev.data = R"({"data":[]})";

    if (!device_property_table_.Upsert(dev)) {
        spdlog::error("CronScheduler: failed to upsert cron device in device_property");
    } else {
        spdlog::info("CronScheduler: cron device registered");
    }
}

void CronScheduler::RegisterCronEvent()
{
    auto evt_id_blob = util::UuidToBlob(kCronEvtUuidStr);
    auto dev_id_blob = util::UuidToBlob(kCronDevUuidStr);

    EventTable::Event evt;
    evt.evt_id = evt_id_blob;
    evt.dev_id = dev_id_blob;
    evt.evt_name = "cron_trigger";
    evt.desc = "Cron job trigger event — fired by the Cron virtual device when a cron expression matches";

    // Build params JSON describing the event parameters
    json params_json;
    params_json["params"] = json::array({
        {{"p_name", "cron_id"}, {"desc", "Cron job UUID"}, {"p_type", "str"}, {"unit", ""}},
        {{"p_name", "expr"}, {"desc", "Cron expression that triggered"}, {"p_type", "str"}, {"unit", ""}},
        {{"p_name", "user_id"}, {"desc", "Optional user ID"}, {"p_type", "str"}, {"unit", ""}}
    });
    evt.params = params_json.dump();

    if (!event_table_.Upsert(evt)) {
        spdlog::error("CronScheduler: failed to upsert cron_trigger event");
    } else {
        spdlog::info("CronScheduler: cron_trigger event registered");
    }
}

// ===========================================================================
// MQTT callback
// ===========================================================================

void CronScheduler::OnAction(const std::string &topic, const std::string &payload)
{
    // Parse the JSON payload.
    json msg;
    try {
        msg = json::parse(payload);
    } catch (const json::parse_error &e) {
        spdlog::error("CronScheduler: failed to parse action payload: {}", e.what());
        return;
    }

    std::string msg_id;
    if (msg.contains("msg_id") && msg["msg_id"].is_string()) {
        msg_id = msg["msg_id"].get<std::string>();
    }

    // Extract act_id from topic: device/<uuid>/action/<act_id>
    std::string act_id;
    auto last_slash = topic.rfind('/');
    if (last_slash != std::string::npos) {
        act_id = topic.substr(last_slash + 1);
    }

    if (act_id.empty()) {
        spdlog::error("CronScheduler: cannot extract act_id from topic: {}", topic);
        SendS2mResponse(msg_id, DeviceRespCode::INVALID_REQUEST);
        return;
    }

    // Extract params sub-object
    json action_params;
    if (msg.contains("params")) {
        action_params = msg["params"];
    }

    spdlog::debug("CronScheduler: received action '{}' msg_id={}", act_id, msg_id);

    // Route to handler
    if (act_id == "add_cron") {
        HandleAddCron(msg_id, action_params);
    } else if (act_id == "add_relative_cron") {
        HandleAddRelativeCron(msg_id, action_params);
    } else if (act_id == "remove_cron") {
        HandleRemoveCron(msg_id, action_params);
    } else if (act_id == "list_crons") {
        HandleListCrons(msg_id);
    } else {
        spdlog::warn("CronScheduler: unknown action '{}'", act_id);
        SendS2mResponse(msg_id, DeviceRespCode::ACTION_NOT_FOUND);
    }
}

// ===========================================================================
// Action handlers
// ===========================================================================

namespace {

// Extract a named parameter value from a params array in the format:
//   [{"p_name": "...", "value": "..."}, ...]
// Returns std::nullopt if the parameter is not found.
std::optional<std::string> GetParamValue(const json &params_array,
                                          const std::string &p_name)
{
    if (!params_array.is_array()) return std::nullopt;
    for (const auto &item : params_array) {
        if (item.contains("p_name") && item["p_name"].is_string()
            && item["p_name"].get<std::string>() == p_name) {
            if (item.contains("value")) {
                if (item["value"].is_string()) {
                    return item["value"].get<std::string>();
                }
                // numbers → string
                if (item["value"].is_number()) {
                    if (item["value"].is_number_float()) {
                        return std::to_string(item["value"].get<double>());
                    }
                    return std::to_string(item["value"].get<int64_t>());
                }
                // fallback: dump whatever it is
                return item["value"].dump();
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // anonymous namespace

void CronScheduler::HandleAddCron(const std::string &msg_id,
                                   const json &action_params)
{
    // Validate expr
    auto expr_opt = GetParamValue(action_params, "expr");
    if (!expr_opt.has_value()) {
        spdlog::warn("CronScheduler: add_cron missing expr");
        SendS2mResponse(msg_id, DeviceRespCode::MISSING_FIELD);
        return;
    }
    std::string expr = *expr_opt;

    // Validate cron expression
    cron::CronExpr parsed;
    std::string parse_err;
    if (!cron::Parse(expr, parsed, &parse_err)) {
        spdlog::warn("CronScheduler: add_cron invalid expr '{}': {}", expr, parse_err);
        json err_data;
        err_data["error"] = parse_err;
        SendS2mResponse(msg_id, DeviceRespCode::INVALID_PARAMS, err_data.dump());
        return;
    }

    // Extract trigger_count (default -1 = infinite)
    int trigger_count = -1;
    auto tc_opt = GetParamValue(action_params, "trigger_count");
    if (tc_opt.has_value()) {
        try { trigger_count = std::stoi(*tc_opt); } catch (...) {}
    }

    // Extract custom params (stored as JSON string by LuaTableToParamsArray)
    std::string custom_params = "{}";
    auto cp_opt = GetParamValue(action_params, "params");
    if (cp_opt.has_value() && !cp_opt->empty()) {
        // Validate that it's parseable JSON, but store as-is
        try {
            json parsed_cp = json::parse(*cp_opt);
            if (parsed_cp.is_object()) {
                custom_params = parsed_cp.dump();
            }
        } catch (...) {
            // If it's not valid JSON, store as a single "value" param
            custom_params = json::object({{"value", *cp_opt}}).dump();
        }
    }

    // Generate cron_id
    std::string cron_id = util::GenerateUuid();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        CronEntry entry;
        entry.id = cron_id;
        entry.expr = expr;
        entry.params = custom_params;
        entry.trigger_count = trigger_count;
        entries_.push_back(std::move(entry));

        if (!SaveCrontab()) {
            spdlog::error("CronScheduler: failed to save crontab after add_cron");
            entries_.pop_back();
            SendS2mResponse(msg_id, DeviceRespCode::INTERNAL_ERROR);
            return;
        }
    }

    cv_.notify_one();

    json data;
    data["cron_id"] = cron_id;
    SendS2mResponse(msg_id, DeviceRespCode::OK, data.dump());

    spdlog::info("CronScheduler: add_cron id={} expr='{}' trigger_count={}",
                 cron_id, expr, trigger_count);
}

void CronScheduler::HandleAddRelativeCron(const std::string &msg_id,
                                           const json &action_params)
{
    // Validate offset
    auto offset_opt = GetParamValue(action_params, "offset");
    if (!offset_opt.has_value()) {
        spdlog::warn("CronScheduler: add_relative_cron missing offset");
        SendS2mResponse(msg_id, DeviceRespCode::MISSING_FIELD);
        return;
    }
    std::string offset_str = *offset_opt;

    // Parse offset
    std::string offset_err;
    auto offset = cron::ParseOffset(offset_str, &offset_err);
    if (!offset.has_value()) {
        spdlog::warn("CronScheduler: add_relative_cron invalid offset '{}': {}",
                     offset_str, offset_err);
        json err_data;
        err_data["error"] = offset_err;
        SendS2mResponse(msg_id, DeviceRespCode::INVALID_PARAMS, err_data.dump());
        return;
    }

    // Compute absolute cron expression
    std::string expr = cron::MakeCronFromOffset(*offset);

    // Extract trigger_count (default 1 = one-shot)
    int trigger_count = 1;
    auto tc_opt = GetParamValue(action_params, "trigger_count");
    if (tc_opt.has_value()) {
        try { trigger_count = std::stoi(*tc_opt); } catch (...) {}
    }

    // Extract custom params
    std::string custom_params = "{}";
    auto cp_opt = GetParamValue(action_params, "params");
    if (cp_opt.has_value() && !cp_opt->empty()) {
        try {
            json parsed_cp = json::parse(*cp_opt);
            if (parsed_cp.is_object()) {
                custom_params = parsed_cp.dump();
            }
        } catch (...) {
            custom_params = json::object({{"value", *cp_opt}}).dump();
        }
    }

    // Generate cron_id
    std::string cron_id = util::GenerateUuid();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        CronEntry entry;
        entry.id = cron_id;
        entry.expr = expr;
        entry.params = custom_params;
        entry.trigger_count = trigger_count;
        entries_.push_back(std::move(entry));

        if (!SaveCrontab()) {
            spdlog::error("CronScheduler: failed to save crontab after add_relative_cron");
            entries_.pop_back();
            SendS2mResponse(msg_id, DeviceRespCode::INTERNAL_ERROR);
            return;
        }
    }

    cv_.notify_one();

    json data;
    data["cron_id"] = cron_id;
    data["expr"] = expr;
    SendS2mResponse(msg_id, DeviceRespCode::OK, data.dump());

    spdlog::info("CronScheduler: add_relative_cron id={} offset='{}' expr='{}' trigger_count={}",
                 cron_id, offset_str, expr, trigger_count);
}

void CronScheduler::HandleRemoveCron(const std::string &msg_id,
                                      const json &action_params)
{
    auto cron_id_opt = GetParamValue(action_params, "cron_id");
    if (!cron_id_opt.has_value()) {
        spdlog::warn("CronScheduler: remove_cron missing cron_id");
        SendS2mResponse(msg_id, DeviceRespCode::MISSING_FIELD);
        return;
    }

    std::string cron_id = *cron_id_opt;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->id == cron_id) {
                entries_.erase(it);
                found = true;
                break;
            }
        }

        last_fired_minute_.erase(cron_id);

        if (found) {
            SaveCrontab();
        }
    }

    // Idempotent — always return OK
    SendS2mResponse(msg_id, DeviceRespCode::OK);

    if (found) {
        spdlog::info("CronScheduler: remove_cron id={}", cron_id);
    } else {
        spdlog::debug("CronScheduler: remove_cron id={} not found (ignored)", cron_id);
    }
}

void CronScheduler::HandleListCrons(const std::string &msg_id)
{
    json data;
    data["jobs"] = json::array();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : entries_) {
            json job;
            job["cron_id"] = entry.id;
            job["expr"] = entry.expr;
            job["trigger_count"] = entry.trigger_count;

            // Parse custom params JSON for display
            if (!entry.params.empty() && entry.params != "{}") {
                try {
                    job["params"] = json::parse(entry.params);
                } catch (...) {
                    job["params"] = entry.params;
                }
            } else {
                job["params"] = json::object();
            }

            data["jobs"].push_back(job);
        }
    }

    SendS2mResponse(msg_id, DeviceRespCode::OK, data.dump());
    spdlog::debug("CronScheduler: list_crons returned {} job(s)",
                  data["jobs"].size());
}

// ===========================================================================
// Scheduler thread
// ===========================================================================

void CronScheduler::SchedulerLoop()
{
    spdlog::info("CronScheduler: scheduler thread started");

    // Pre-compute the cron_trigger event and device UUID BLOBs once.
    auto evt_id_blob = util::UuidToBlob(kCronEvtUuidStr);
    auto dev_id_blob = util::UuidToBlob(kCronDevUuidStr);

    while (running_) {
        // Wait up to 30 seconds, or until notified (e.g. new job added).
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(30));
        }

        if (!running_) break;

        // Get current UTC+8 time.
        int minute = 0, hour = 0, day = 0, month = 0, dow = 0;
        cron::GetCurrentTime(minute, hour, day, month, dow);

        // Compute current unix minute for dedup.
        auto now = std::chrono::system_clock::now();
        auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        int64_t current_minute_key = now_sec / 60;

        // Snapshot entries under lock.
        std::vector<CronEntry> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = entries_;
        }

        bool modified = false;

        for (auto &entry : snapshot) {
            // Dedup: skip if already fired this minute.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = last_fired_minute_.find(entry.id);
                if (it != last_fired_minute_.end() && it->second == current_minute_key) {
                    continue;
                }
            }

            // Parse the cron expression.
            cron::CronExpr parsed;
            if (!cron::Parse(entry.expr, parsed)) {
                spdlog::warn("CronScheduler: failed to parse expr '{}' for job {}",
                             entry.expr, entry.id);
                continue;
            }

            // Check if current time matches.
            if (!cron::Matches(parsed, minute, hour, day, month, dow)) {
                continue;
            }

            // --- Fire! ---

            // Record dedup.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_fired_minute_[entry.id] = current_minute_key;
            }

            // Build params_json for InjectEvent.
            // Format: [{"p_name":"...","value":"..."}, ...]
            json params_array = json::array();

            // System params
            params_array.push_back({{"p_name", "cron_id"}, {"value", entry.id}});
            params_array.push_back({{"p_name", "expr"}, {"value", entry.expr}});

            // Custom params from stored JSON
            if (!entry.params.empty() && entry.params != "{}") {
                try {
                    json custom = json::parse(entry.params);
                    for (auto &[key, val] : custom.items()) {
                        std::string val_str;
                        if (val.is_string()) {
                            val_str = val.get<std::string>();
                        } else if (val.is_number()) {
                            // Convert number to string for rule engine compatibility
                            if (val.is_number_float()) {
                                val_str = std::to_string(val.get<double>());
                            } else {
                                val_str = std::to_string(val.get<int64_t>());
                            }
                        } else if (val.is_boolean()) {
                            val_str = val.get<bool>() ? "true" : "false";
                        } else {
                            val_str = val.dump();
                        }
                        params_array.push_back({{"p_name", key}, {"value", val_str}});
                    }
                } catch (...) {
                    spdlog::warn("CronScheduler: failed to parse custom params for job {}",
                                 entry.id);
                }
            }

            std::string params_json = params_array.dump();

            spdlog::info("CronScheduler: firing job {} expr='{}' trigger_count={}",
                         entry.id, entry.expr, entry.trigger_count);

            // Inject event into the rule engine.
            rule_engine_->InjectEvent(evt_id_blob, dev_id_blob,
                                      "cron_trigger", params_json);

            // Handle trigger count.
            if (entry.trigger_count > 0) {
                entry.trigger_count--;
                modified = true;

                if (entry.trigger_count == 0) {
                    spdlog::info("CronScheduler: job {} trigger_count reached 0, removing",
                                 entry.id);
                    // Mark for removal (will be removed below).
                }
            }
        }

        // Apply trigger_count changes and remove depleted entries.
        if (modified) {
            std::lock_guard<std::mutex> lock(mutex_);

            // Build a set of IDs to remove.
            std::vector<std::string> to_remove;
            for (auto &snap_entry : snapshot) {
                if (snap_entry.trigger_count == 0) {
                    to_remove.push_back(snap_entry.id);
                }
            }

            // Update counts and remove depleted entries.
            for (auto &entry : entries_) {
                for (auto &snap_entry : snapshot) {
                    if (entry.id == snap_entry.id) {
                        entry.trigger_count = snap_entry.trigger_count;
                        break;
                    }
                }
            }

            entries_.erase(
                std::remove_if(entries_.begin(), entries_.end(),
                               [&to_remove](const CronEntry &e) {
                                   return std::find(to_remove.begin(),
                                                    to_remove.end(),
                                                    e.id) != to_remove.end();
                               }),
                entries_.end());

            for (const auto &id : to_remove) {
                last_fired_minute_.erase(id);
            }

            SaveCrontab();
        }
    }

    spdlog::info("CronScheduler: scheduler thread exited");
}

// ===========================================================================
// crontab.txt persistence
// ===========================================================================

bool CronScheduler::LoadCrontab()
{
    entries_.clear();

    std::ifstream file(crontab_path_);
    if (!file.is_open()) {
        // No crontab yet — that's fine for first run.
        spdlog::info("CronScheduler: no existing crontab at {}", crontab_path_);
        return true;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        line_no++;

        // Skip empty lines and comments.
        if (line.empty() || line[0] == '#') continue;

        // Parse pipe-delimited format: <uuid>|<expr>|<params_json>|<trigger_count>
        // Find the first three pipes.
        auto p1 = line.find('|');
        if (p1 == std::string::npos) {
            spdlog::warn("CronScheduler: crontab line {} malformed (missing field 1): {}", line_no, line);
            continue;
        }

        auto p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos) {
            spdlog::warn("CronScheduler: crontab line {} malformed (missing field 2): {}", line_no, line);
            continue;
        }

        auto p3 = line.find('|', p2 + 1);
        if (p3 == std::string::npos) {
            spdlog::warn("CronScheduler: crontab line {} malformed (missing field 3): {}", line_no, line);
            continue;
        }

        CronEntry entry;
        entry.id = line.substr(0, p1);
        entry.expr = line.substr(p1 + 1, p2 - p1 - 1);
        entry.params = line.substr(p2 + 1, p3 - p2 - 1);

        std::string count_str = line.substr(p3 + 1);
        try {
            entry.trigger_count = std::stoi(count_str);
        } catch (...) {
            spdlog::warn("CronScheduler: crontab line {} invalid trigger_count '{}', using -1",
                         line_no, count_str);
            entry.trigger_count = -1;
        }

        // Validate the cron expression on load.
        cron::CronExpr parsed;
        if (!cron::Parse(entry.expr, parsed)) {
            spdlog::warn("CronScheduler: crontab line {} has invalid expr '{}', skipping",
                         line_no, entry.expr);
            continue;
        }

        entries_.push_back(std::move(entry));
    }

    return true;
}

bool CronScheduler::SaveCrontab()
{
    std::ofstream file(crontab_path_);
    if (!file.is_open()) {
        spdlog::error("CronScheduler: failed to open crontab for writing: {}", crontab_path_);
        return false;
    }

    for (const auto &entry : entries_) {
        file << entry.id << '|'
             << entry.expr << '|'
             << entry.params << '|'
             << entry.trigger_count << '\n';
    }

    bool ok = file.good();
    if (!ok) {
        spdlog::error("CronScheduler: error writing crontab");
    }
    return ok;
}

// ===========================================================================
// S2M response (device → host)
// ===========================================================================

void CronScheduler::SendS2mResponse(const std::string &msg_id,
                                     DeviceRespCode code,
                                     const std::string &data_json)
{
    json reply;
    reply["msg_id"] = msg_id;
    reply["resp"] = static_cast<int>(code);

    if (!data_json.empty()) {
        try {
            reply["data"] = json::parse(data_json);
        } catch (...) {
            reply["data"] = data_json;
        }
    }

    std::string topic = std::string("device/") + kCronDevUuidStr + "/resp/s2m";
    if (!mqtt_client_->PublishMessage(topic, reply.dump(), /*qos=*/1)) {
        spdlog::error("CronScheduler: failed to send s2m reply (resp={})",
                      static_cast<int>(code));
        return;
    }

    spdlog::debug("CronScheduler: sent s2m reply resp={} msg_id={}",
                  static_cast<int>(code), msg_id);
}

}  // namespace cortexlink
