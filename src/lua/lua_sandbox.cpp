#include "lua/lua_sandbox.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#include "db/device_data_table.h"
#include "db/device_property_table.h"
#include "db/rule_table.h"
#include "llm/open_claw_client.h"
#include "mqtt/mqtt_client.h"
#include "util/uuid_util.h"

namespace cortexlink {

// ===========================================================================
// Constructor
// ===========================================================================

LuaSandbox::LuaSandbox(DeviceDataTable *device_data_table,
                       DevicePropertyTable *device_property_table,
                       MqttClient *mqtt_client,
                       RuleTable *rule_table,
                       OpenClawClient *open_claw_client)
    : device_data_table_(device_data_table)
    , device_property_table_(device_property_table)
    , mqtt_client_(mqtt_client)
    , rule_table_(rule_table)
    , open_claw_client_(open_claw_client)
{
}

// ===========================================================================
// Memory Allocator
// ===========================================================================

void *LuaSandbox::LuaAlloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    auto *ctx = static_cast<LuaStateContext *>(ud);

    if (nsize == 0) {
        // Free
        if (ptr != nullptr) {
            ctx->memory_used -= osize;
            std::free(ptr);
        }
        return nullptr;
    }

    if (ptr == nullptr) {
        // New allocation
        if (ctx->memory_used + nsize > ctx->memory_limit) {
            return nullptr;  // would exceed limit
        }
        void *result = std::malloc(nsize);
        if (result != nullptr) {
            ctx->memory_used += nsize;
        }
        return result;
    }

    // Resize existing block
    if (nsize > osize) {
        size_t delta = nsize - osize;
        if (ctx->memory_used + delta > ctx->memory_limit) {
            return nullptr;  // would exceed limit
        }
    }

    void *result = std::realloc(ptr, nsize);
    if (result != nullptr) {
        // Adjust tracked usage: nsize may be smaller than osize
        ctx->memory_used = ctx->memory_used - osize + nsize;
    }
    // If realloc fails, old block is still valid — don't adjust counts
    return result;
}

// ===========================================================================
// Instruction Hook
// ===========================================================================

void LuaSandbox::InstructionHook(lua_State *L, lua_Debug * /*ar*/)
{
    void *ud = nullptr;
    lua_getallocf(L, &ud);
    auto *ctx = static_cast<LuaStateContext *>(ud);

    ctx->instruction_count += kHookInterval;

    // Check instruction limit
    if (ctx->instruction_count > kMaxInstructions) {
        ctx->timed_out = true;
        luaL_error(L, "execution timeout: instruction limit exceeded");
        return;  // unreachable — luaL_error does longjmp
    }

    // Check wall-clock limit
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - ctx->start_time).count();
    if (elapsed >= kTimeoutMs) {
        ctx->timed_out = true;
        luaL_error(L, "execution timeout: time limit exceeded");
    }
}

// ===========================================================================
// Read-Only Table Support
// ===========================================================================

int LuaSandbox::ReadOnlyError(lua_State *L)
{
    return luaL_error(L, "attempt to modify a read-only table");
}

void LuaSandbox::MakeReadOnly(lua_State *L, int tbl_idx)
{
    tbl_idx = lua_absindex(L, tbl_idx);

    // Create metatable with __newindex that raises an error.
    lua_newtable(L);
    lua_pushcfunction(L, ReadOnlyError);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, tbl_idx);

    // Recursively protect sub-tables.
    lua_pushnil(L);
    while (lua_next(L, tbl_idx) != 0) {
        if (lua_istable(L, -1)) {
            MakeReadOnly(L, lua_gettop(L));
        }
        lua_pop(L, 1);  // pop value, keep key
    }
}

// ===========================================================================
// Table Helpers
// ===========================================================================

void LuaSandbox::SetField(lua_State *L, const char *key, const std::string &value)
{
    lua_pushstring(L, key);
    lua_pushstring(L, value.c_str());
    lua_settable(L, -3);
}

void LuaSandbox::SetField(lua_State *L, const char *key, int value)
{
    lua_pushstring(L, key);
    lua_pushinteger(L, value);
    lua_settable(L, -3);
}

// ===========================================================================
// Lua Table → JSON Conversion
// ===========================================================================

namespace {

nlohmann::json LuaTableToJson(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    nlohmann::json result = nlohmann::json::object();

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        // key at -2, value at -1
        std::string key;
        if (lua_isstring(L, -2)) {
            key = lua_tostring(L, -2);
        } else if (lua_isinteger(L, -2)) {
            key = std::to_string(lua_tointeger(L, -2));
        } else {
            lua_pop(L, 1);
            continue;
        }

        if (lua_isinteger(L, -1)) {
            result[key] = lua_tointeger(L, -1);
        } else if (lua_isnumber(L, -1)) {
            result[key] = lua_tonumber(L, -1);
        } else if (lua_isstring(L, -1)) {
            result[key] = lua_tostring(L, -1);
        } else if (lua_isboolean(L, -1)) {
            result[key] = static_cast<bool>(lua_toboolean(L, -1));
        } else if (lua_istable(L, -1)) {
            result[key] = LuaTableToJson(L, lua_gettop(L));
        } else {
            result[key] = luaL_tolstring(L, -1, nullptr);
            lua_pop(L, 1);  // luaL_tolstring pushes a copy
        }

        lua_pop(L, 1);  // pop value
    }

    return result;
}

// Convert a Lua table {key = value, ...} to the standard params array format:
//   [{"p_name": "key", "value": "string_value"}, ...]
// All values are converted to strings. Nested tables are JSON-stringified.
nlohmann::json LuaTableToParamsArray(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    nlohmann::json result = nlohmann::json::array();

    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        // key at -2, value at -1
        std::string key;
        if (lua_isstring(L, -2)) {
            key = lua_tostring(L, -2);
        } else if (lua_isinteger(L, -2)) {
            key = std::to_string(lua_tointeger(L, -2));
        } else {
            lua_pop(L, 1);
            continue;
        }

        std::string val_str;
        if (lua_isinteger(L, -1)) {
            val_str = std::to_string(lua_tointeger(L, -1));
        } else if (lua_isnumber(L, -1)) {
            val_str = std::to_string(lua_tonumber(L, -1));
        } else if (lua_isstring(L, -1)) {
            val_str = lua_tostring(L, -1);
        } else if (lua_isboolean(L, -1)) {
            val_str = lua_toboolean(L, -1) ? "true" : "false";
        } else if (lua_istable(L, -1)) {
            nlohmann::json nested = LuaTableToJson(L, lua_gettop(L));
            val_str = nested.dump();
        } else {
            val_str = luaL_tolstring(L, -1, nullptr);
            lua_pop(L, 1);
        }

        result.push_back({{"p_name", key}, {"value", val_str}});

        lua_pop(L, 1);  // pop value
    }

    return result;
}

}  // anonymous namespace

// ===========================================================================
// Host API: get_data(dev_uuid, data_name) → value | nil
// ===========================================================================

int LuaSandbox::GetDataFn(lua_State *L)
{
    auto *self = static_cast<LuaSandbox *>(lua_touserdata(L, lua_upvalueindex(1)));

    const char *dev_uuid_str = luaL_checkstring(L, 1);
    const char *data_name = luaL_checkstring(L, 2);

    auto dev_id_blob = util::UuidToBlob(dev_uuid_str);
    auto result = self->device_data_table_->Get(dev_id_blob, data_name);

    if (!result.has_value()) {
        lua_pushnil(L);
        return 1;
    }

    const auto &dd = result.value();
    if (dd.data_type == "int") {
        try {
            lua_pushinteger(L, std::stoll(dd.data_val));
        } catch (const std::exception &) {
            lua_pushinteger(L, 0);
        }
    } else if (dd.data_type == "float") {
        try {
            lua_pushnumber(L, std::stod(dd.data_val));
        } catch (const std::exception &) {
            lua_pushnumber(L, 0.0);
        }
    } else {
        // "str" or unknown — treat as string
        lua_pushstring(L, dd.data_val.c_str());
    }

    return 1;
}

// ===========================================================================
// Host API: do_action(dev_uuid, action_id, params) → true|false, err_msg
// ===========================================================================

int LuaSandbox::DoActionFn(lua_State *L)
{
    auto *self = static_cast<LuaSandbox *>(lua_touserdata(L, lua_upvalueindex(1)));

    const char *dev_uuid_str = luaL_checkstring(L, 1);
    const char *action_id = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);

    // Check device exists and is online
    auto dev_id_blob = util::UuidToBlob(dev_uuid_str);
    auto dev = self->device_property_table_->GetByDevId(dev_id_blob);

    if (!dev.has_value()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "device not found");
        return 2;
    }

    if (dev->dev_state != "online") {
        lua_pushboolean(L, false);
        lua_pushstring(L, "device is offline");
        return 2;
    }

    // Build JSON payload
    nlohmann::json payload;
    payload["msg_id"] = util::GenerateUuid();
    payload["act_id"] = action_id;
    payload["params"] = LuaTableToParamsArray(L, 3);

    // Publish to device/{uuid}/action/{action_id}
    std::string topic = "device/" + std::string(dev_uuid_str)
                        + "/action/" + std::string(action_id);

    if (!self->mqtt_client_->PublishMessage(topic, payload.dump(), /*qos=*/1)) {
        spdlog::error("LuaSandbox: do_action publish failed — topic={}", topic);
        lua_pushboolean(L, false);
        lua_pushstring(L, "MQTT publish failed");
        return 2;
    }

    spdlog::debug("LuaSandbox: do_action — dev={} act_id={} topic={}",
                  dev_uuid_str, action_id, topic);

    lua_pushboolean(L, true);
    return 1;
}

// ===========================================================================
// Host API: publish(topic_suffix, payload) → true|false, err_msg
// ===========================================================================

int LuaSandbox::PublishFn(lua_State *L)
{
    auto *self = static_cast<LuaSandbox *>(lua_touserdata(L, lua_upvalueindex(1)));

    const char *topic_arg = luaL_checkstring(L, 1);
    std::string topic(topic_arg);

    // Security check: forbid broadcast/sql/, broadcast/config and app/llm/
    if (topic.rfind("broadcast/sql/", 0) == 0 ||
        topic == "broadcast/config" ||
        topic.rfind("app/llm/", 0) == 0) {
        spdlog::warn("LuaSandbox: publish to forbidden topic '{}' blocked", topic);
        lua_pushboolean(L, false);
        lua_pushstring(L, "forbidden topic");
        return 2;
    }

    // Convert payload (string or table) to string
    std::string payload_str;
    if (lua_isstring(L, 2)) {
        payload_str = lua_tostring(L, 2);
    } else if (lua_istable(L, 2)) {
        payload_str = LuaTableToJson(L, 2).dump();
    } else {
        payload_str = luaL_tolstring(L, 2, nullptr);
        lua_pop(L, 1);
    }

    if (!self->mqtt_client_->PublishMessage(topic, payload_str)) {
        spdlog::error("LuaSandbox: publish failed — topic={}", topic);
        lua_pushboolean(L, false);
        lua_pushstring(L, "MQTT publish failed");
        return 2;
    }

    spdlog::debug("LuaSandbox: publish to topic={}", topic);

    lua_pushboolean(L, true);
    return 1;
}

// ===========================================================================
// Host API: log(level, message)
// ===========================================================================

int LuaSandbox::LogFn(lua_State *L)
{
    const char *level_str = luaL_checkstring(L, 1);
    const char *message = luaL_checkstring(L, 2);

    std::string level(level_str);

    if (level == "DEBUG") {
        spdlog::debug("[lua] {}", message);
    } else if (level == "INFO") {
        spdlog::info("[lua] {}", message);
    } else if (level == "WARN") {
        spdlog::warn("[lua] {}", message);
    } else if (level == "ERROR") {
        spdlog::error("[lua] {}", message);
    } else {
        spdlog::info("[lua] {}", message);
    }

    return 0;
}

// ===========================================================================
// Host API: get_device_state(dev_uuid) → "online"|"offline" | nil
// ===========================================================================

int LuaSandbox::GetDeviceStateFn(lua_State *L)
{
    auto *self = static_cast<LuaSandbox *>(lua_touserdata(L, lua_upvalueindex(1)));

    const char *dev_uuid_str = luaL_checkstring(L, 1);
    auto dev_id_blob = util::UuidToBlob(dev_uuid_str);
    auto dev = self->device_property_table_->GetByDevId(dev_id_blob);

    if (!dev.has_value()) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, dev->dev_state.c_str());
    return 1;
}

// ===========================================================================
// Host API: request_rule(prompt, session) → true | false, err_msg
// ===========================================================================

int LuaSandbox::RequestRuleFn(lua_State *L)
{
    auto *self = static_cast<LuaSandbox *>(lua_touserdata(L, lua_upvalueindex(1)));

    const char *prompt = luaL_checkstring(L, 1);

    // session is optional (nil or string) — defaults to empty string.
    const char *session = luaL_optstring(L, 2, "");

    if (!self->open_claw_client_) {
        spdlog::error("LuaSandbox: request_rule — OpenClawClient is null");
        lua_pushboolean(L, false);
        lua_pushstring(L, "OpenClaw client not initialized");
        return 2;
    }

    if (!self->open_claw_client_->SendMessage(session, prompt)) {
        spdlog::warn("LuaSandbox: request_rule failed — session='{}' prompt_len={}",
                     session, std::strlen(prompt));
        lua_pushboolean(L, false);
        lua_pushstring(L, "OpenClaw request failed");
        return 2;
    }

    spdlog::debug("LuaSandbox: request_rule sent — session='{}' prompt_len={}",
                  session, std::strlen(prompt));

    lua_pushboolean(L, true);
    return 1;
}

// ===========================================================================
// State Creation
// ===========================================================================

lua_State *LuaSandbox::CreateState(LuaStateContext *ctx)
{
    lua_State *L = lua_newstate(LuaAlloc, ctx);
    if (L == nullptr) {
        spdlog::error("LuaSandbox: failed to create lua_State (out of memory)");
        return nullptr;
    }

    // Open base library and remove dangerous functions
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);

    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushnil(L);
    lua_setglobal(L, "load");

    // Open allowed libraries
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);

    // Register host API functions
    RegisterHostApi(L);

    return L;
}

// ===========================================================================
// Host API Registration
// ===========================================================================

void LuaSandbox::RegisterHostApi(lua_State *L)
{
    // Each function gets `this` as a lightuserdata upvalue so it can reach
    // the dependency pointers.

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, GetDataFn, 1);
    lua_setglobal(L, "get_data");

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, DoActionFn, 1);
    lua_setglobal(L, "do_action");

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, PublishFn, 1);
    lua_setglobal(L, "publish");

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, LogFn, 1);
    lua_setglobal(L, "log");

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, GetDeviceStateFn, 1);
    lua_setglobal(L, "get_device_state");

    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, RequestRuleFn, 1);
    lua_setglobal(L, "request_rule");
}

// ===========================================================================
// Global Variable Injection
// ===========================================================================

void LuaSandbox::InjectEvent(lua_State *L, const EventContext &evt)
{
    lua_newtable(L);                        // event table at top

    // Scalar fields
    SetField(L, "evt_id", evt.evt_id);
    SetField(L, "evt_name", evt.evt_name);
    SetField(L, "dev_id", evt.dev_id);

    // params sub-table
    lua_pushstring(L, "params");
    lua_newtable(L);
    for (const auto &[key, value] : evt.params) {
        lua_pushstring(L, key.c_str());
        lua_pushstring(L, value.c_str());
        lua_settable(L, -3);
    }
    MakeReadOnly(L, lua_gettop(L));         // protect params
    lua_settable(L, -3);                    // event.params = params_table

    MakeReadOnly(L, lua_gettop(L));         // protect event
    lua_setglobal(L, "event");
}

void LuaSandbox::InjectTime(lua_State *L)
{
    // Get current time in UTC+8
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    t += 8 * 3600;  // UTC+8 offset
    std::tm *tm = std::gmtime(&t);

    char hms_buf[16];
    std::snprintf(hms_buf, sizeof(hms_buf), "%02d:%02d:%02d",
                  tm->tm_hour, tm->tm_min, tm->tm_sec);

    lua_newtable(L);                        // time table at top

    SetField(L, "hour", tm->tm_hour);
    SetField(L, "min", tm->tm_min);
    SetField(L, "sec", tm->tm_sec);
    SetField(L, "hms", std::string(hms_buf));

    MakeReadOnly(L, lua_gettop(L));         // protect time
    lua_setglobal(L, "time");
}

// ===========================================================================
// Execute
// ===========================================================================

bool LuaSandbox::Execute(const std::string &script,
                         const EventContext &evt,
                         int64_t rule_id)
{
    // 1. Script length check
    if (script.size() > kMaxScriptLength) {
        spdlog::error("LuaSandbox: script exceeds max length ({} > {})",
                      script.size(), kMaxScriptLength);
        return false;
    }

    // 2. Attempt execution (up to 2 attempts)
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Allocate fresh context on the heap — survives longjmp out of Lua.
        auto ctx = std::make_unique<LuaStateContext>();
        ctx->sandbox = this;
        ctx->start_time = std::chrono::steady_clock::now();
        ctx->instruction_count = 0;
        ctx->timed_out = false;

        // Create isolated Lua state
        lua_State *L = CreateState(ctx.get());
        if (L == nullptr) {
            spdlog::error("LuaSandbox: failed to create Lua state (attempt {})",
                          attempt + 1);
            return false;
        }

        // Inject read-only globals
        InjectEvent(L, evt);
        InjectTime(L);

        // Set instruction-count hook
        lua_sethook(L, InstructionHook, LUA_MASKCOUNT, kHookInterval);

        // Load script
        int load_ret = luaL_loadstring(L, script.c_str());
        if (load_ret != LUA_OK) {
            // Syntax error — no retry
            const char *err = lua_tostring(L, -1);
            spdlog::error("LuaSandbox: syntax error in rule {}: {}",
                          rule_id, err ? err : "unknown");
            lua_close(L);
            return false;
        }

        // Execute
        int pcall_ret = lua_pcall(L, 0, 0, 0);

        if (pcall_ret == LUA_OK) {
            // Success
            lua_close(L);
            int64_t new_count = rule_table_->IncrementCount(rule_id);
            spdlog::info("LuaSandbox: rule {} executed successfully (count={})",
                         rule_id, new_count);
            return true;
        }

        // Execution failed — check reason
        const char *err_msg = lua_tostring(L, -1);
        bool timed_out = ctx->timed_out;

        lua_close(L);

        if (pcall_ret == LUA_ERRMEM) {
            spdlog::error("LuaSandbox: rule {} out of memory: {}",
                          rule_id, err_msg ? err_msg : "memory limit exceeded");
            return false;
        }

        if (timed_out) {
            if (attempt == 0) {
                spdlog::warn("LuaSandbox: rule {} timed out — {}; retrying in {}ms",
                             rule_id,
                             err_msg ? err_msg : "timeout",
                             kRetryDelayMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
                continue;  // → fresh lua_State on next iteration
            }

            // Double timeout — disable the rule
            spdlog::error("LuaSandbox: rule {} timed out twice ({}); disabling rule",
                          rule_id, err_msg ? err_msg : "timeout");
            rule_table_->SetEnable(rule_id, false);
            return false;
        }

        // Runtime error (type error, nil access, etc.) — no retry
        spdlog::error("LuaSandbox: rule {} runtime error: {}",
                      rule_id, err_msg ? err_msg : "unknown");
        return false;
    }

    return false;
}

}  // namespace cortexlink
