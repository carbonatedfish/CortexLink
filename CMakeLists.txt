cmake_minimum_required(VERSION 3.12)
project(CortexLink)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# config Lua 5.4
find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED lua5.4)
# end

# config mosquitto
find_library(MOSQUITTOPP_LIB mosquittopp REQUIRED)
find_path(MOSQUITTOPP_INCLUDE_DIR mosquittopp.h REQUIRED)
# end

set(CortexLink_MQTT_SRC
    src/mqtt/mqtt_client.cpp
)

set(CortexLink_DB_SRC 
    src/db/db_table.cpp
    src/db/device_property_table.cpp
    src/db/event_table.cpp
    src/db/rule_table.cpp
    src/db/device_data_table.cpp
    src/db/user_profile_table.cpp
    src/db/event_rule_table.cpp
    src/db/event_record_table.cpp
)

set(CortexLink_UTIL_SRC
    src/util/uuid_util.cpp
    src/util/log_util.cpp
)

set(CortexLink_RULE_ENGINE_SRC
    src/rule_engine/cond_parser.cpp
    src/rule_engine/rule_engine.cpp
)

set(CortexLink_DEVICE_SRC
    src/device/device_manager.cpp
)

set(CortexLink_LUA_SRC
    src/lua/lua_sandbox.cpp
)

# --- OpenSSL (for SHA-256 checksum) ---
find_library(CRYPTO_LIB crypto REQUIRED)

# --- App module source ---
set(CortexLink_APP_SRC
    src/app/app_manager.cpp
    src/app/sql_strategy.cpp
    src/app/app_sql_proxy.cpp
)

# --- LLM SQL proxy module source ---
set(CortexLink_LLM_SQL_SRC
    src/llm/llm_sql_strategy.cpp
    src/llm/llm_sql_proxy.cpp
    src/llm/open_claw_client.cpp
)

add_executable(CortexLink
    src/main.cpp
    ${CortexLink_MQTT_SRC}
    ${CortexLink_UTIL_SRC}
    ${CortexLink_DB_SRC}
    ${CortexLink_RULE_ENGINE_SRC}
    ${CortexLink_DEVICE_SRC}
    ${CortexLink_LUA_SRC}
    ${CortexLink_APP_SRC}
    ${CortexLink_LLM_SQL_SRC}
)
target_include_directories(CortexLink PRIVATE include include/3rdparty ${MOSQUITTOPP_INCLUDE_DIR} ${LUA_INCLUDE_DIRS})
target_link_libraries(CortexLink PRIVATE ${MOSQUITTOPP_LIB} Threads::Threads SQLite::SQLite3 ${LUA_LIBRARIES} ${CRYPTO_LIB})

# --- Test executable ---
add_executable(CortexLinkTest
    test/rule_engine_test.cpp
    ${CortexLink_MQTT_SRC}
    ${CortexLink_UTIL_SRC}
    ${CortexLink_DB_SRC}
    ${CortexLink_RULE_ENGINE_SRC}
    ${CortexLink_DEVICE_SRC}
    ${CortexLink_LUA_SRC}
    src/llm/open_claw_client.cpp
)
target_include_directories(CortexLinkTest PRIVATE include include/3rdparty ${MOSQUITTOPP_INCLUDE_DIR} ${LUA_INCLUDE_DIRS})
target_link_libraries(CortexLinkTest PRIVATE ${MOSQUITTOPP_LIB} Threads::Threads SQLite::SQLite3 ${LUA_LIBRARIES} ${CRYPTO_LIB})