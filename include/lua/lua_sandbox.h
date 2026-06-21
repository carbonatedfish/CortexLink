#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

struct lua_State;
struct lua_Debug;

namespace cortexlink {

// Forward declarations for injected dependencies.
class DeviceDataTable;
class DevicePropertyTable;
class MqttClient;
class RuleTable;

// LuaSandbox creates an isolated Lua 5.4 execution environment per script
// invocation. It enforces resource limits (8 MB memory, 5 M instructions,
// 3 s wall-clock timeout) and exposes a curated set of host API functions
// to Lua scripts: get_data, do_action, publish, log, get_device_state.
//
// Dependencies are injected via the constructor as raw pointers (the caller
// guarantees they outlive the LuaSandbox instance).
//
// Usage:
//   LuaSandbox sandbox(&dev_data, &dev_prop, &mqtt, &rules);
//   LuaSandbox::EventContext evt{...};
//   sandbox.Execute(rule.action, evt, rule.rule_id);
class LuaSandbox {
public:
    // Context describing the event that triggered rule evaluation.
    // Injected into Lua as the read-only global `event`.
    struct EventContext {
        std::string evt_id;     // UUID string (hyphenated)
        std::string evt_name;   // e.g. "motion", "cron_trigger"
        std::string dev_id;     // UUID string of the originating device
        std::unordered_map<std::string, std::string> params;  // param_name → value
    };

    // Current wall-clock time (UTC+8), injected as read-only global `time`.
    struct TimeInfo {
        int hour = 0;           // 0–23
        int min = 0;            // 0–59
        int sec = 0;            // 0–59
        std::string hms;        // "HH:MM:SS"
    };

    LuaSandbox(DeviceDataTable *device_data_table,
               DevicePropertyTable *device_property_table,
               MqttClient *mqtt_client,
               RuleTable *rule_table);

    ~LuaSandbox() = default;

    LuaSandbox(const LuaSandbox &) = delete;
    LuaSandbox &operator=(const LuaSandbox &) = delete;

    // Execute a Lua script triggered by the given event.
    //
    // Returns true if the script completed successfully (rule.count
    // incremented). Returns false on syntax error, runtime error, or
    // double-timeout; in the double-timeout case rule.enable is set to 0.
    //
    // The script must not exceed kMaxScriptLength (4096) characters.
    bool Execute(const std::string &script,
                 const EventContext &evt,
                 int64_t rule_id);

    // ---- resource limits (also used internally by static helpers) ----------
    static constexpr size_t kMaxScriptLength = 4096;
    static constexpr int64_t kMaxInstructions = 5'000'000;
    static constexpr int kHookInterval = 100'000;
    static constexpr int kTimeoutMs = 3'000;
    static constexpr int kRetryDelayMs = 1'000;
    static constexpr size_t kMemoryLimit = 8 * 1024 * 1024;  // 8 MB

private:
    // Per-invocation mutable state. Allocated on the heap and attached to
    // the lua_State via lua_newstate so the allocator and hook can access it.
    struct LuaStateContext {
        size_t memory_used = 0;
        size_t memory_limit = kMemoryLimit;
        LuaSandbox *sandbox = nullptr;

        std::chrono::steady_clock::time_point start_time;
        int64_t instruction_count = 0;
        bool timed_out = false;
    };

    // Create a fully sandboxed lua_State with resource limits and host API.
    lua_State *CreateState(LuaStateContext *ctx);

    // Register the five host API functions as global Lua closures.
    void RegisterHostApi(lua_State *L);

    // Inject the read-only `event` and `time` global tables.
    void InjectEvent(lua_State *L, const EventContext &evt);
    void InjectTime(lua_State *L);

    // Make the table at stack index tbl_idx read-only (recursively).
    static void MakeReadOnly(lua_State *L, int tbl_idx);

    // Helper: push a string key + value onto the stack and set table.
    static void SetField(lua_State *L, const char *key, const std::string &value);
    static void SetField(lua_State *L, const char *key, int value);

    // ---- host API C functions (registered as global closures) ------------
    static int GetDataFn(lua_State *L);
    static int DoActionFn(lua_State *L);
    static int PublishFn(lua_State *L);
    static int LogFn(lua_State *L);
    static int GetDeviceStateFn(lua_State *L);

    // ---- sandbox enforcement ----------------------------------------------
    static void *LuaAlloc(void *ud, void *ptr, size_t osize, size_t nsize);
    static void InstructionHook(lua_State *L, lua_Debug *ar);
    static int ReadOnlyError(lua_State *L);

    // ---- injected dependencies (not owned) --------------------------------
    DeviceDataTable *device_data_table_;
    DevicePropertyTable *device_property_table_;
    MqttClient *mqtt_client_;
    RuleTable *rule_table_;
};

}  // namespace cortexlink
