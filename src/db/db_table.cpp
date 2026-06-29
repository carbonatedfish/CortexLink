#include "db/db_table.h"

#include <spdlog/spdlog.h>

#include <filesystem>

namespace cortexlink {

sqlite3 *DBTable::db_ = nullptr;
std::mutex DBTable::write_mutex_;

bool DBTable::Initialize(const std::string &db_path)
{
    if (db_) {
        spdlog::warn("DBTable: database already initialized");
        return true;
    }

    // Ensure parent directory exists
    std::filesystem::path path(db_path);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        spdlog::error("DBTable: failed to create directory '{}': {}",
                      path.parent_path().string(), ec.message());
        return false;
    }

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        spdlog::error("DBTable: sqlite3_open '{}' failed: {}", db_path,
                      sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Enable WAL mode for better concurrent read performance
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    spdlog::info("DBTable: database opened at '{}'", db_path);
    return true;
}

void DBTable::Shutdown()
{
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        spdlog::info("DBTable: database closed");
    }
}

sqlite3 *DBTable::GetDB()
{
    return db_;
}

bool DBTable::ExecuteWrite(const std::string &sql)
{
    std::lock_guard<std::mutex> lock(write_mutex_);

    spdlog::debug("DBTable: executing write SQL: {}", sql);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("DBTable: prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        spdlog::error("DBTable: step failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DBTable::ExecuteWrite(
    const std::string &sql,
    const std::function<int(sqlite3_stmt *)> &bind_fn)
{
    std::lock_guard<std::mutex> lock(write_mutex_);

    spdlog::debug("DBTable: executing write SQL (with bind): {}", sql);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("DBTable: prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    if (bind_fn) {
        rc = bind_fn(stmt);
        if (rc != SQLITE_OK) {
            spdlog::error("DBTable: bind failed: {}", sqlite3_errmsg(db_));
            sqlite3_finalize(stmt);
            return false;
        }
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        spdlog::error("DBTable: step failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DBTable::ExecuteRead(const std::string &sql)
{
    std::lock_guard<std::mutex> lock(write_mutex_);

    spdlog::debug("DBTable: executing read SQL: {}", sql);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("DBTable: prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // For reads, SQLITE_ROW or SQLITE_DONE are both OK
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        spdlog::error("DBTable: step failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DBTable::ExecuteRead(
    const std::string &sql,
    const std::function<int(sqlite3_stmt *)> &bind_fn,
    const std::function<void(sqlite3_stmt *)> &row_fn)
{
    std::lock_guard<std::mutex> lock(write_mutex_);

    spdlog::debug("DBTable: executing read SQL (with bind): {}", sql);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("DBTable: prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    if (bind_fn) {
        rc = bind_fn(stmt);
        if (rc != SQLITE_OK) {
            spdlog::error("DBTable: bind failed: {}", sqlite3_errmsg(db_));
            sqlite3_finalize(stmt);
            return false;
        }
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (row_fn) {
            row_fn(stmt);
        }
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        spdlog::error("DBTable: step failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

}  // namespace cortexlink
