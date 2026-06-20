#pragma once

#include <vector>

#include "db/db_table.h"

namespace cortexlink {

class EventRuleTable : public DBTable {
public:
    struct EventRule {
        std::array<uint8_t, 16> evt_id;
        int64_t rule_id = 0;
    };

    bool CreateTable() override;

    // Bind an event to a rule.
    bool Insert(const EventRule &er);

    // Remove all bindings for a rule.
    bool DeleteByRuleId(int64_t rule_id);

    // Remove all bindings for an event.
    bool DeleteByEvtId(const std::array<uint8_t, 16> &evt_id);

    // Get all rule IDs triggered by this event.
    std::vector<int64_t> GetRulesByEvtId(
        const std::array<uint8_t, 16> &evt_id);

    // Get all event IDs bound to this rule.
    std::vector<std::array<uint8_t, 16>> GetEventsByRuleId(int64_t rule_id);
};

}  // namespace cortexlink
