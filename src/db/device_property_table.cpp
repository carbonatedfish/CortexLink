#include "db/device_property_table.h"

#include <spdlog/spdlog.h>

namespace cortexlink {

bool DevicePropertyTable::CreateTable()
{
    const char *sql = R"SQL(
        CREATE TABLE IF NOT EXISTS device_property (
            dev_id     BLOB PRIMARY KEY,
            dev_name   TEXT NOT NULL,
            dev_type   TEXT NOT NULL,
            dev_subtype TEXT,
            dev_state  TEXT NOT NULL,
            location   TEXT,
            user_param TEXT,
            actions    TEXT,
            events     TEXT,
            data       TEXT
        );
    )SQL";
    return ExecuteWrite(sql);
}

bool DevicePropertyTable::Insert(const DeviceProperty &dev)
{
    const char *sql = R"SQL(
        INSERT INTO device_property (dev_id, dev_name, dev_type, dev_subtype,
                                     dev_state, location, user_param,
                                     actions, events, data)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )SQL";

    auto bind = [&dev](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev.dev_id.data(),
                          static_cast<int>(dev.dev_id.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, dev.dev_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, dev.dev_type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, dev.dev_subtype.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, dev.dev_state.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, dev.location.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 7, dev.user_param.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 8, dev.actions.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 9, dev.events.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 10, dev.data.c_str(), -1, SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool DevicePropertyTable::Update(const DeviceProperty &dev)
{
    const char *sql = R"SQL(
        UPDATE device_property
        SET dev_name=?, dev_type=?, dev_subtype=?, dev_state=?,
            location=?, user_param=?, actions=?, events=?, data=?
        WHERE dev_id=?;
    )SQL";

    auto bind = [&dev](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_text(stmt, 1, dev.dev_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, dev.dev_type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, dev.dev_subtype.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, dev.dev_state.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, dev.location.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, dev.user_param.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 7, dev.actions.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 8, dev.events.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 9, dev.data.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 10, dev.dev_id.data(),
                          static_cast<int>(dev.dev_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool DevicePropertyTable::Upsert(const DeviceProperty &dev)
{
    const char *sql = R"SQL(
        INSERT OR REPLACE INTO device_property
            (dev_id, dev_name, dev_type, dev_subtype,
             dev_state, location, user_param, actions, events, data)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )SQL";

    auto bind = [&dev](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev.dev_id.data(),
                          static_cast<int>(dev.dev_id.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, dev.dev_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, dev.dev_type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, dev.dev_subtype.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, dev.dev_state.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, dev.location.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 7, dev.user_param.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 8, dev.actions.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 9, dev.events.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 10, dev.data.c_str(), -1, SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool DevicePropertyTable::Delete(const std::array<uint8_t, 16> &dev_id)
{
    const char *sql = "DELETE FROM device_property WHERE dev_id=?;";

    auto bind = [&dev_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

std::optional<DevicePropertyTable::DeviceProperty>
DevicePropertyTable::GetByDevId(const std::array<uint8_t, 16> &dev_id)
{
    const char *sql = "SELECT * FROM device_property WHERE dev_id=?;";

    std::optional<DeviceProperty> result;

    auto bind = [&dev_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    auto row = [&result](sqlite3_stmt *stmt) {
        DeviceProperty dev;
        const void *blob = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size == 16) {
            std::memcpy(dev.dev_id.data(), blob, 16);
        }
        dev.dev_name   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        dev.dev_type   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        dev.dev_subtype = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3)) ?: "";
        dev.dev_state  = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        dev.location   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5)) ?: "";
        dev.user_param = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6)) ?: "";
        dev.actions    = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7)) ?: "";
        dev.events     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8)) ?: "";
        dev.data       = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 9)) ?: "";
        result = std::move(dev);
    };

    ExecuteRead(sql, bind, row);
    return result;
}

std::vector<DevicePropertyTable::DeviceProperty> DevicePropertyTable::GetAll()
{
    const char *sql = "SELECT * FROM device_property;";
    std::vector<DeviceProperty> results;

    auto row = [&results](sqlite3_stmt *stmt) {
        DeviceProperty dev;
        const void *blob = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size == 16) {
            std::memcpy(dev.dev_id.data(), blob, 16);
        }
        dev.dev_name   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        dev.dev_type   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        dev.dev_subtype = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3)) ?: "";
        dev.dev_state  = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        dev.location   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5)) ?: "";
        dev.user_param = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6)) ?: "";
        dev.actions    = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7)) ?: "";
        dev.events     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8)) ?: "";
        dev.data       = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 9)) ?: "";
        results.push_back(std::move(dev));
    };

    ExecuteRead(sql, nullptr, row);
    return results;
}

bool DevicePropertyTable::UpdateState(
    const std::array<uint8_t, 16> &dev_id, const std::string &state)
{
    const char *sql = "UPDATE device_property SET dev_state=? WHERE dev_id=?;";

    auto bind = [&dev_id, &state](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_text(stmt, 1, state.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 2, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool DevicePropertyTable::UpdateConfigFields(
    const std::array<uint8_t, 16> &dev_id,
    const std::optional<std::string> &dev_name,
    const std::optional<std::string> &location,
    const std::optional<std::string> &user_param)
{
    if (!dev_name.has_value() && !location.has_value() && !user_param.has_value()) {
        spdlog::warn("DevicePropertyTable::UpdateConfigFields: no fields to update");
        return false;
    }

    // Build SET clause dynamically. Only hardcoded column names are
    // concatenated into the SQL; all user-supplied values go through
    // sqlite3_bind_text — safe from SQL injection.
    std::string sql = "UPDATE device_property SET ";
    if (dev_name.has_value())   sql += "dev_name=?,";
    if (location.has_value())   sql += "location=?,";
    if (user_param.has_value()) sql += "user_param=?,";
    sql.pop_back();  // remove trailing comma
    sql += " WHERE dev_id=?;";

    auto bind = [&dev_id, &dev_name, &location, &user_param](sqlite3_stmt *stmt) -> int {
        int idx = 1;
        if (dev_name.has_value()) {
            sqlite3_bind_text(stmt, idx++, dev_name->c_str(), -1, SQLITE_STATIC);
        }
        if (location.has_value()) {
            sqlite3_bind_text(stmt, idx++, location->c_str(), -1, SQLITE_STATIC);
        }
        if (user_param.has_value()) {
            sqlite3_bind_text(stmt, idx++, user_param->c_str(), -1, SQLITE_STATIC);
        }
        sqlite3_bind_blob(stmt, idx, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    if (!ExecuteWrite(sql, bind)) {
        spdlog::error("DevicePropertyTable::UpdateConfigFields: SQL failed");
        return false;
    }

    spdlog::debug("DevicePropertyTable::UpdateConfigFields: success");
    return true;
}

}  // namespace cortexlink
