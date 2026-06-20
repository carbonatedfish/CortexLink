#include "db/event_record_table.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace cortexlink {

bool EventRecordTable::CreateTable()
{
    const char *sql = R"SQL(
        CREATE TABLE IF NOT EXISTS event_record (
            id       INTEGER PRIMARY KEY AUTOINCREMENT,
            evt_id   BLOB NOT NULL,
            dev_id   BLOB NOT NULL,
            evt_name TEXT NOT NULL,
            params   TEXT,
            time     DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )SQL";
    return ExecuteWrite(sql);
}

bool EventRecordTable::Insert(EventRecord &record)
{
    const char *sql = R"SQL(
        INSERT INTO event_record (evt_id, dev_id, evt_name, params, time)
        VALUES (?, ?, ?, ?, datetime('now', 'localtime'));
    )SQL";

    std::lock_guard<std::mutex> lock(write_mutex_);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("EventRecordTable: prepare insert failed: {}",
                      sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_blob(stmt, 1, record.evt_id.data(),
                      static_cast<int>(record.evt_id.size()), SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, record.dev_id.data(),
                      static_cast<int>(record.dev_id.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, record.evt_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, record.params.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        spdlog::error("EventRecordTable: insert step failed: {}",
                      sqlite3_errmsg(db_));
        return false;
    }

    record.id = sqlite3_last_insert_rowid(db_);
    return true;
}

std::vector<EventRecordTable::EventRecord> EventRecordTable::GetByDevId(
    const std::array<uint8_t, 16> &dev_id, int limit)
{
    const char *sql = R"SQL(
        SELECT id, evt_id, dev_id, evt_name, params, time
        FROM event_record
        WHERE dev_id=?
        ORDER BY time DESC
        LIMIT ?;
    )SQL";

    std::vector<EventRecord> results;

    auto bind = [&dev_id, limit](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, dev_id.data(),
                          static_cast<int>(dev_id.size()), SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, limit);
        return SQLITE_OK;
    };

    auto row = [&results](sqlite3_stmt *stmt) {
        EventRecord rec;
        rec.id = sqlite3_column_int64(stmt, 0);

        const void *blob1 = sqlite3_column_blob(stmt, 1);
        int size1 = sqlite3_column_bytes(stmt, 1);
        if (blob1 && size1 == 16) std::memcpy(rec.evt_id.data(), blob1, 16);

        const void *blob2 = sqlite3_column_blob(stmt, 2);
        int size2 = sqlite3_column_bytes(stmt, 2);
        if (blob2 && size2 == 16) std::memcpy(rec.dev_id.data(), blob2, 16);

        rec.evt_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        rec.params   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)) ?: "";
        rec.time     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5)) ?: "";

        results.push_back(std::move(rec));
    };

    ExecuteRead(sql, bind, row);
    return results;
}

std::vector<EventRecordTable::EventRecord> EventRecordTable::GetByTimeRange(
    const std::string &start, const std::string &end)
{
    const char *sql = R"SQL(
        SELECT id, evt_id, dev_id, evt_name, params, time
        FROM event_record
        WHERE time >= ? AND time <= ?
        ORDER BY time DESC;
    )SQL";

    std::vector<EventRecord> results;

    auto bind = [&start, &end](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_text(stmt, 1, start.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, end.c_str(), -1, SQLITE_STATIC);
        return SQLITE_OK;
    };

    auto row = [&results](sqlite3_stmt *stmt) {
        EventRecord rec;
        rec.id = sqlite3_column_int64(stmt, 0);

        const void *blob1 = sqlite3_column_blob(stmt, 1);
        int size1 = sqlite3_column_bytes(stmt, 1);
        if (blob1 && size1 == 16) std::memcpy(rec.evt_id.data(), blob1, 16);

        const void *blob2 = sqlite3_column_blob(stmt, 2);
        int size2 = sqlite3_column_bytes(stmt, 2);
        if (blob2 && size2 == 16) std::memcpy(rec.dev_id.data(), blob2, 16);

        rec.evt_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        rec.params   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)) ?: "";
        rec.time     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5)) ?: "";

        results.push_back(std::move(rec));
    };

    ExecuteRead(sql, bind, row);
    return results;
}

std::vector<EventRecordTable::EventRecord> EventRecordTable::GetAll(int limit)
{
    const char *sql = R"SQL(
        SELECT id, evt_id, dev_id, evt_name, params, time
        FROM event_record
        ORDER BY time DESC
        LIMIT ?;
    )SQL";

    std::vector<EventRecord> results;

    auto bind = [limit](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_int(stmt, 1, limit);
        return SQLITE_OK;
    };

    auto row = [&results](sqlite3_stmt *stmt) {
        EventRecord rec;
        rec.id = sqlite3_column_int64(stmt, 0);

        const void *blob1 = sqlite3_column_blob(stmt, 1);
        int size1 = sqlite3_column_bytes(stmt, 1);
        if (blob1 && size1 == 16) std::memcpy(rec.evt_id.data(), blob1, 16);

        const void *blob2 = sqlite3_column_blob(stmt, 2);
        int size2 = sqlite3_column_bytes(stmt, 2);
        if (blob2 && size2 == 16) std::memcpy(rec.dev_id.data(), blob2, 16);

        rec.evt_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        rec.params   = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)) ?: "";
        rec.time     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5)) ?: "";

        results.push_back(std::move(rec));
    };

    ExecuteRead(sql, bind, row);
    return results;
}

}  // namespace cortexlink
