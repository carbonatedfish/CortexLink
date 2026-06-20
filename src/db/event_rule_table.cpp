#include "db/event_rule_table.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace cortexlink {

bool EventRuleTable::CreateTable()
{
    const char *sql = R"SQL(
        CREATE TABLE IF NOT EXISTS event_rule (
            evt_id  BLOB NOT NULL,
            rule_id INTEGER NOT NULL,
            PRIMARY KEY(evt_id, rule_id),
            FOREIGN KEY(evt_id) REFERENCES event(evt_id),
            FOREIGN KEY(rule_id) REFERENCES rule(rule_id)
        );
    )SQL";
    return ExecuteWrite(sql);
}

bool EventRuleTable::Insert(const EventRule &er)
{
    const char *sql = "INSERT OR IGNORE INTO event_rule (evt_id, rule_id) VALUES (?, ?);";

    auto bind = [&er](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, er.evt_id.data(),
                          static_cast<int>(er.evt_id.size()), SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, er.rule_id);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool EventRuleTable::DeleteByRuleId(int64_t rule_id)
{
    const char *sql = "DELETE FROM event_rule WHERE rule_id=?;";

    auto bind = [rule_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_int64(stmt, 1, rule_id);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool EventRuleTable::DeleteByEvtId(const std::array<uint8_t, 16> &evt_id)
{
    const char *sql = "DELETE FROM event_rule WHERE evt_id=?;";

    auto bind = [&evt_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, evt_id.data(),
                          static_cast<int>(evt_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

std::vector<int64_t> EventRuleTable::GetRulesByEvtId(
    const std::array<uint8_t, 16> &evt_id)
{
    const char *sql = "SELECT rule_id FROM event_rule WHERE evt_id=?;";

    std::vector<int64_t> results;

    auto bind = [&evt_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, evt_id.data(),
                          static_cast<int>(evt_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    auto row = [&results](sqlite3_stmt *stmt) {
        results.push_back(sqlite3_column_int64(stmt, 0));
    };

    ExecuteRead(sql, bind, row);
    return results;
}

std::vector<std::array<uint8_t, 16>> EventRuleTable::GetEventsByRuleId(
    int64_t rule_id)
{
    const char *sql = "SELECT evt_id FROM event_rule WHERE rule_id=?;";

    std::vector<std::array<uint8_t, 16>> results;

    auto bind = [rule_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_int64(stmt, 1, rule_id);
        return SQLITE_OK;
    };

    auto row = [&results](sqlite3_stmt *stmt) {
        std::array<uint8_t, 16> evt_id{};
        const void *blob = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size == 16) {
            std::memcpy(evt_id.data(), blob, 16);
        }
        results.push_back(evt_id);
    };

    ExecuteRead(sql, bind, row);
    return results;
}

}  // namespace cortexlink
