#include "db/event_table.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace cortexlink {

bool EventTable::CreateTable()
{
    const char *sql = R"SQL(
        CREATE TABLE IF NOT EXISTS event (
            evt_id   BLOB PRIMARY KEY,
            dev_id   BLOB NOT NULL,
            evt_name TEXT NOT NULL,
            "desc"   TEXT,
            params   TEXT,
            FOREIGN KEY(dev_id) REFERENCES device_property(dev_id)
        );
    )SQL";
    return ExecuteWrite(sql);
}

bool EventTable::Insert(const Event &evt)
{
    const char *sql = R"SQL(
        INSERT INTO event (evt_id, dev_id, evt_name, "desc", params)
        VALUES (?, ?, ?, ?, ?);
    )SQL";

    auto bind = [&evt](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, evt.evt_id.data(),
                          static_cast<int>(evt.evt_id.size()), SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 2, evt.dev_id.data(),
                          static_cast<int>(evt.dev_id.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, evt.evt_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, evt.desc.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, evt.params.c_str(), -1, SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool EventTable::Delete(const std::array<uint8_t, 16> &evt_id)
{
    const char *sql = "DELETE FROM event WHERE evt_id=?;";

    auto bind = [&evt_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, evt_id.data(),
                          static_cast<int>(evt_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

std::optional<EventTable::Event> EventTable::GetByEvtId(
    const std::array<uint8_t, 16> &evt_id)
{
    const char *sql = R"SQL(SELECT evt_id, dev_id, evt_name, "desc", params FROM event WHERE evt_id=?;)SQL";

    std::optional<Event> result;

    auto bind = [&evt_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, evt_id.data(),
                          static_cast<int>(evt_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    auto row = [&result](sqlite3_stmt *stmt) {
        Event evt;

        const void *blob0 = sqlite3_column_blob(stmt, 0);
        int size0 = sqlite3_column_bytes(stmt, 0);
        if (blob0 && size0 == 16) std::memcpy(evt.evt_id.data(), blob0, 16);

        const void *blob1 = sqlite3_column_blob(stmt, 1);
        int size1 = sqlite3_column_bytes(stmt, 1);
        if (blob1 && size1 == 16) std::memcpy(evt.dev_id.data(), blob1, 16);

        evt.evt_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        evt.desc     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3)) ?: "";
        evt.params   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)) ?: "";

        result = std::move(evt);
    };

    ExecuteRead(sql, bind, row);
    return result;
}

std::vector<EventTable::Event> EventTable::GetByDevId(
    const std::array<uint8_t, 16> &dev_id)
{
    const char *sql = R"SQL(SELECT evt_id, dev_id, evt_name, "desc", params FROM event WHERE dev_id=?;)SQL";

    std::vector<Event> results;

    auto bind = [&dev_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    auto row = [&results](sqlite3_stmt *stmt) {
        Event evt;

        const void *blob0 = sqlite3_column_blob(stmt, 0);
        int size0 = sqlite3_column_bytes(stmt, 0);
        if (blob0 && size0 == 16) std::memcpy(evt.evt_id.data(), blob0, 16);

        const void *blob1 = sqlite3_column_blob(stmt, 1);
        int size1 = sqlite3_column_bytes(stmt, 1);
        if (blob1 && size1 == 16) std::memcpy(evt.dev_id.data(), blob1, 16);

        evt.evt_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        evt.desc     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3)) ?: "";
        evt.params   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)) ?: "";

        results.push_back(std::move(evt));
    };

    ExecuteRead(sql, bind, row);
    return results;
}

std::vector<EventTable::Event> EventTable::GetAll()
{
    const char *sql = R"SQL(SELECT evt_id, dev_id, evt_name, "desc", params FROM event;)SQL";

    std::vector<Event> results;

    auto row = [&results](sqlite3_stmt *stmt) {
        Event evt;

        const void *blob0 = sqlite3_column_blob(stmt, 0);
        int size0 = sqlite3_column_bytes(stmt, 0);
        if (blob0 && size0 == 16) std::memcpy(evt.evt_id.data(), blob0, 16);

        const void *blob1 = sqlite3_column_blob(stmt, 1);
        int size1 = sqlite3_column_bytes(stmt, 1);
        if (blob1 && size1 == 16) std::memcpy(evt.dev_id.data(), blob1, 16);

        evt.evt_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        evt.desc     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3)) ?: "";
        evt.params   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)) ?: "";

        results.push_back(std::move(evt));
    };

    ExecuteRead(sql, nullptr, row);
    return results;
}

}  // namespace cortexlink
