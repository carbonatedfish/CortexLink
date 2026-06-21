#pragma once

#include <string>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace cortexlink {

// Abstract strategy: one per cmd.
// Each strategy encapsulates a SQL template and parameter binding logic.
class SqlStrategy {
public:
    virtual ~SqlStrategy() = default;

    // Return the SQL template string (uses ? positional placeholders).
    // params is available for strategies that select SQL dynamically.
    virtual std::string GetSql(const nlohmann::json &params) const = 0;

    // Validate that `params` contains all required fields.
    // Default: always valid (for parameterless queries).
    virtual bool ValidateParams(const nlohmann::json &params) const;

    // Bind parameters from JSON to the prepared statement.
    // Called after sqlite3_prepare_v2, before sqlite3_step.
    // Indexing is 1-based and follows the ? order in GetSql().
    // Return true on success.
    virtual bool BindParams(sqlite3_stmt *stmt,
                            const nlohmann::json &params) const = 0;

protected:
    // Helper: bind a UUID string from params[`key`] as a 16-byte BLOB
    // at the given 1-based index. Returns true on success.
    static bool BindUuidParam(sqlite3_stmt *stmt, int idx,
                              const nlohmann::json &params,
                              const std::string &key);

    // Helper: bind an int64 from params[`key`] at the given index.
    // Returns true on success.
    static bool BindIntParam(sqlite3_stmt *stmt, int idx,
                             const nlohmann::json &params,
                             const std::string &key);
};

// ---- Concrete strategies ------------------------------------------------

// cmd: get_device_list
class GetDeviceListStrategy : public SqlStrategy {
public:
    std::string GetSql(const nlohmann::json &) const override;
    bool BindParams(sqlite3_stmt *, const nlohmann::json &) const override;
};

// cmd: get_device_detail
class GetDeviceDetailStrategy : public SqlStrategy {
public:
    std::string GetSql(const nlohmann::json &) const override;
    bool ValidateParams(const nlohmann::json &params) const override;
    bool BindParams(sqlite3_stmt *, const nlohmann::json &) const override;
};

// cmd: get_device_data
class GetDeviceDataStrategy : public SqlStrategy {
public:
    std::string GetSql(const nlohmann::json &) const override;
    bool ValidateParams(const nlohmann::json &params) const override;
    bool BindParams(sqlite3_stmt *, const nlohmann::json &) const override;
};

// cmd: get_rules
class GetRulesStrategy : public SqlStrategy {
public:
    std::string GetSql(const nlohmann::json &) const override;
    bool BindParams(sqlite3_stmt *, const nlohmann::json &) const override;
};

// cmd: get_rule_detail
class GetRuleDetailStrategy : public SqlStrategy {
public:
    std::string GetSql(const nlohmann::json &) const override;
    bool ValidateParams(const nlohmann::json &params) const override;
    bool BindParams(sqlite3_stmt *, const nlohmann::json &) const override;
};

// cmd: get_user_profiles
class GetUserProfilesStrategy : public SqlStrategy {
public:
    std::string GetSql(const nlohmann::json &) const override;
    bool BindParams(sqlite3_stmt *, const nlohmann::json &) const override;
};

}  // namespace cortexlink
