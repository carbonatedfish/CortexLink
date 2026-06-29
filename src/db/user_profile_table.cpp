#include "db/user_profile_table.h"

#include <cstring>

#include <spdlog/spdlog.h>

#include "util/uuid_util.h"

namespace cortexlink {

bool UserProfileTable::CreateTable()
{
    const char *sql = R"SQL(
        CREATE TABLE IF NOT EXISTS user_profile (
            user_id    BLOB PRIMARY KEY,
            user_name  TEXT NOT NULL,
            preference TEXT
        );
    )SQL";
    return ExecuteWrite(sql);
}

bool UserProfileTable::Insert(const UserProfile &user)
{
    const char *sql = R"SQL(
        INSERT INTO user_profile (user_id, user_name, preference)
        VALUES (?, ?, ?);
    )SQL";

    auto bind = [&user](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, user.user_id.data(),
                          static_cast<int>(user.user_id.size()), SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, user.user_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, user.preference.c_str(), -1, SQLITE_STATIC);
        return SQLITE_OK;
    };

    spdlog::debug("UserProfileTable: insert user_id={} name='{}'",
                  util::BlobToUuid(user.user_id), user.user_name);
    return ExecuteWrite(sql, bind);
}

bool UserProfileTable::Update(const UserProfile &user)
{
    const char *sql = R"SQL(
        UPDATE user_profile SET user_name=?, preference=? WHERE user_id=?;
    )SQL";

    auto bind = [&user](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_text(stmt, 1, user.user_name.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, user.preference.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_blob(stmt, 3, user.user_id.data(),
                          static_cast<int>(user.user_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

bool UserProfileTable::Delete(const std::array<uint8_t, 16> &user_id)
{
    const char *sql = "DELETE FROM user_profile WHERE user_id=?;";

    auto bind = [&user_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, user_id.data(),
                          static_cast<int>(user_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    return ExecuteWrite(sql, bind);
}

std::optional<UserProfileTable::UserProfile> UserProfileTable::GetByUserId(
    const std::array<uint8_t, 16> &user_id)
{
    const char *sql = "SELECT user_id, user_name, preference FROM user_profile WHERE user_id=?;";

    std::optional<UserProfile> result;

    auto bind = [&user_id](sqlite3_stmt *stmt) -> int {
        sqlite3_bind_blob(stmt, 1, user_id.data(),
                          static_cast<int>(user_id.size()), SQLITE_STATIC);
        return SQLITE_OK;
    };

    auto row = [&result](sqlite3_stmt *stmt) {
        UserProfile user;

        const void *blob = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size == 16) std::memcpy(user.user_id.data(), blob, 16);

        user.user_name  = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        user.preference = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)) ?: "";

        result = std::move(user);
    };

    ExecuteRead(sql, bind, row);
    spdlog::debug("UserProfileTable: lookup user_id={} found={}",
                  util::BlobToUuid(user_id), result.has_value());
    return result;
}

std::vector<UserProfileTable::UserProfile> UserProfileTable::GetAll()
{
    const char *sql = "SELECT user_id, user_name, preference FROM user_profile;";

    std::vector<UserProfile> results;

    auto row = [&results](sqlite3_stmt *stmt) {
        UserProfile user;

        const void *blob = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (blob && size == 16) std::memcpy(user.user_id.data(), blob, 16);

        user.user_name  = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        user.preference = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)) ?: "";

        results.push_back(std::move(user));
    };

    ExecuteRead(sql, nullptr, row);
    return results;
}

}  // namespace cortexlink
