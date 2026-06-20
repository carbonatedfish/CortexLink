#include "db/device_data_table.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace cortexlink {

bool DeviceDataTable::CreateTable()
{
    const char *sql = R"SQL(
        CREATE TABLE IF NOT EXISTS device_data (
            dev_id    BLOB NOT NULL,
            data_name TEXT NOT NULL,
            data_type TEXT NOT NULL,
            data_val  TEXT NOT NULL,
            PRIMARY KEY(dev_id, data_name),
            FOREIGN KEY(dev_id) REFERENCES device_property(dev_id)
        );
    )SQL";
    return ExecuteWrite(sql);
}

bool DeviceDataTable::Upsert(const DeviceData &data)
{
    const char *sql = R"SQL(
        INSERT OR REPLACE INTO device_data (dev_id, data_name, data_type, data_val)
        VALUES (?, ?, ?, ?);
    )SQL";

    auto bind = [&data](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, data.dev_id.data(),
                          static_cast<int>(data.dev_id.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, data.data_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, data.data_type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, data.data_val.c_str(), -1, SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

std::optional<DeviceDataTable::DeviceData> DeviceDataTable::Get(
    const std::array<uint8_t, 16> &dev_id, const std::string &data_name)
{
    const char *sql = "SELECT dev_id, data_name, data_type, data_val FROM device_data WHERE dev_id=? AND data_name=?;";

    std::optional<DeviceData> result;

    auto bind = [&dev_id, &data_name](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, data_name.c_str(), -1, SQLITE_STATIC);
        return SQLITE_OK;
    };

    auto row = [&result](sqlite3_stmt *stmt) {
        DeviceData data;

        const void *blob = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size == 16) std::memcpy(data.dev_id.data(), blob, 16);

        data.data_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        data.data_type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        data.data_val  = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));

        result = std::move(data);
    };

    ExecuteRead(sql, bind, row);
    return result;
}

std::vector<DeviceDataTable::DeviceData> DeviceDataTable::GetByDevId(
    const std::array<uint8_t, 16> &dev_id)
{
    const char *sql = "SELECT dev_id, data_name, data_type, data_val FROM device_data WHERE dev_id=?;";

    std::vector<DeviceData> results;

    auto bind = [&dev_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    auto row = [&results](sqlite3_stmt *stmt) {
        DeviceData data;

        const void *blob = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size == 16) std::memcpy(data.dev_id.data(), blob, 16);

        data.data_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        data.data_type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        data.data_val  = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));

        results.push_back(std::move(data));
    };

    ExecuteRead(sql, bind, row);
    return results;
}

bool DeviceDataTable::Delete(const std::array<uint8_t, 16> &dev_id,
                             const std::string &data_name)
{
    const char *sql = "DELETE FROM device_data WHERE dev_id=? AND data_name=?;";

    auto bind = [&dev_id, &data_name](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, data_name.c_str(), -1, SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

}  // namespace cortexlink
