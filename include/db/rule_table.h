#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db/db_table.h"

namespace cortexlink {

class RuleTable : public DBTable {
public:
    struct Rule {
        int64_t rule_id = 0;       // auto-increment primary key
        std::string rule_name;
        std::string rule_type;     // automation / reminder / schedule etc.
        bool enable = true;
        int64_t count = 0;
        int64_t limit = 0;         // 0 = unlimited
        std::string cond_expr;     // condition expression (may be empty)
        std::string action;        // Lua script text
    };

    bool CreateTable() override;

    // Insert a new rule. On success, sets rule.rule_id to the new row id.
    bool Insert(Rule &rule);

    bool Update(const Rule &rule);
    bool Delete(int64_t rule_id);

    std::optional<Rule> GetByRuleId(int64_t rule_id);
    std::vector<Rule> GetAll();

    // Get only enabled rules.
    std::vector<Rule> GetEnabled();

    // Toggle enable flag.
    bool SetEnable(int64_t rule_id, bool enable);

    // Atomically increment count and return the new value.
    // Returns -1 on failure.
    int64_t IncrementCount(int64_t rule_id);
};

}  // namespace cortexlink
