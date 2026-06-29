#include "db/rule_table.h"

#include <spdlog/spdlog.h>

namespace cortexlink {

bool RuleTable::CreateTable()
{
    const char *sql = R"SQL(
        CREATE TABLE IF NOT EXISTS rule (
            rule_id   INTEGER PRIMARY KEY AUTOINCREMENT,
            rule_name TEXT NOT NULL,
            rule_type TEXT NOT NULL,
            enable    INTEGER NOT NULL DEFAULT 1,
            count     INTEGER DEFAULT 0,
            "limit"   INTEGER DEFAULT 0,
            cond_expr TEXT,
            action    TEXT NOT NULL
        );
    )SQL";
    return ExecuteWrite(sql);
}

bool RuleTable::Insert(Rule &rule)
{
    const char *sql = R"SQL(
        INSERT INTO rule (rule_name, rule_type, enable, count, "limit", cond_expr, action)
        VALUES (?, ?, ?, ?, ?, ?, ?);
    )SQL";

    std::lock_guard<std::mutex> lock(write_mutex_);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("RuleTable: prepare insert failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, rule.rule_name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, rule.rule_type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, rule.enable ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, rule.count);
    sqlite3_bind_int64(stmt, 5, rule.limit);
    sqlite3_bind_text(stmt, 6, rule.cond_expr.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, rule.action.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        spdlog::error("RuleTable: insert step failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    rule.rule_id = sqlite3_last_insert_rowid(db_);
    spdlog::debug("RuleTable: insert rule_id={} name='{}'", rule.rule_id, rule.rule_name);
    return true;
}

bool RuleTable::Update(const Rule &rule)
{
    const char *sql = R"SQL(
        UPDATE rule
        SET rule_name=?, rule_type=?, enable=?, count=?, "limit"=?, cond_expr=?, action=?
        WHERE rule_id=?;
    )SQL";

    auto bind = [&rule](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_text(stmt, 1, rule.rule_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, rule.rule_type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, rule.enable ? 1 : 0);
        sqlite3_bind_int64(stmt, 4, rule.count);
        sqlite3_bind_int64(stmt, 5, rule.limit);
        sqlite3_bind_text(stmt, 6, rule.cond_expr.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 7, rule.action.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 8, rule.rule_id);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool RuleTable::Delete(int64_t rule_id)
{
    const char *sql = "DELETE FROM rule WHERE rule_id=?;";

    auto bind = [rule_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_int64(stmt, 1, rule_id);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

std::optional<RuleTable::Rule> RuleTable::GetByRuleId(int64_t rule_id)
{
    const char *sql = R"SQL(SELECT rule_id, rule_name, rule_type, enable, count, "limit", cond_expr, action FROM rule WHERE rule_id=?;)SQL";

    std::optional<Rule> result;

    auto bind = [rule_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_int64(stmt, 1, rule_id);
        return SQLITE_OK;
    };

    auto row = [&result](sqlite3_stmt *stmt) {
        Rule rule;
        rule.rule_id   = sqlite3_column_int64(stmt, 0);
        rule.rule_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        rule.rule_type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        rule.enable    = sqlite3_column_int(stmt, 3) != 0;
        rule.count     = sqlite3_column_int64(stmt, 4);
        rule.limit     = sqlite3_column_int64(stmt, 5);
        rule.cond_expr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6)) ?: "";
        rule.action    = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
        result = std::move(rule);
    };

    ExecuteRead(sql, bind, row);
    return result;
}

std::vector<RuleTable::Rule> RuleTable::GetAll()
{
    const char *sql = R"SQL(SELECT rule_id, rule_name, rule_type, enable, count, "limit", cond_expr, action FROM rule;)SQL";

    std::vector<Rule> results;

    auto row = [&results](sqlite3_stmt *stmt) {
        Rule rule;
        rule.rule_id   = sqlite3_column_int64(stmt, 0);
        rule.rule_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        rule.rule_type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        rule.enable    = sqlite3_column_int(stmt, 3) != 0;
        rule.count     = sqlite3_column_int64(stmt, 4);
        rule.limit     = sqlite3_column_int64(stmt, 5);
        rule.cond_expr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6)) ?: "";
        rule.action    = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
        results.push_back(std::move(rule));
    };

    ExecuteRead(sql, nullptr, row);
    return results;
}

std::vector<RuleTable::Rule> RuleTable::GetEnabled()
{
    const char *sql = R"SQL(SELECT rule_id, rule_name, rule_type, enable, count, "limit", cond_expr, action FROM rule WHERE enable=1;)SQL";

    std::vector<Rule> results;

    auto row = [&results](sqlite3_stmt *stmt) {
        Rule rule;
        rule.rule_id   = sqlite3_column_int64(stmt, 0);
        rule.rule_name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        rule.rule_type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        rule.enable    = sqlite3_column_int(stmt, 3) != 0;
        rule.count     = sqlite3_column_int64(stmt, 4);
        rule.limit     = sqlite3_column_int64(stmt, 5);
        rule.cond_expr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6)) ?: "";
        rule.action    = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
        results.push_back(std::move(rule));
    };

    ExecuteRead(sql, nullptr, row);
    return results;
}

bool RuleTable::SetEnable(int64_t rule_id, bool enable)
{
    const char *sql = "UPDATE rule SET enable=? WHERE rule_id=?;";

    auto bind = [rule_id, enable](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_int(stmt, 1, enable ? 1 : 0);
        sqlite3_bind_int64(stmt, 2, rule_id);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

int64_t RuleTable::IncrementCount(int64_t rule_id)
{
    const char *sql = "UPDATE rule SET count = count + 1 WHERE rule_id=?;";

    std::lock_guard<std::mutex> lock(write_mutex_);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("RuleTable: increment prepare failed: {}", sqlite3_errmsg(db_));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, rule_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        spdlog::error("RuleTable: increment step failed: {}", sqlite3_errmsg(db_));
        return -1;
    }

    // Read back the new count
    const char *read_sql = "SELECT count FROM rule WHERE rule_id=?;";
    int64_t new_count = -1;

    sqlite3_stmt *r_stmt = nullptr;
    rc = sqlite3_prepare_v2(db_, read_sql, -1, &r_stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("RuleTable: read count prepare failed: {}", sqlite3_errmsg(db_));
        return -1;
    }

    sqlite3_bind_int64(r_stmt, 1, rule_id);

    if (sqlite3_step(r_stmt) == SQLITE_ROW) {
        new_count = sqlite3_column_int64(r_stmt, 0);
    }

    sqlite3_finalize(r_stmt);
    spdlog::debug("RuleTable: increment rule_id={} new_count={}", rule_id, new_count);
    return new_count;
}

}  // namespace cortexlink
