#pragma once

#include <string>
#include <vector>

#include "db/db_table.h"

namespace cortexlink {

class EventRecordTable : public DBTable {
public:
    struct EventRecord {
        int64_t id = 0;                // auto-increment
        std::array<uint8_t, 16> evt_id;
        std::array<uint8_t, 16> dev_id;
        std::string evt_name;
        std::string params;            // JSON string of actual params
        std::string time;              // DATETIME string
    };

    bool CreateTable() override;

    // Insert a new event record. On success, sets record.id.
    bool Insert(EventRecord &record);

    // Get records for a device, ordered by time descending, with optional limit.
    std::vector<EventRecord> GetByDevId(const std::array<uint8_t, 16> &dev_id,
                                        int limit = 100);

    // Get records in a time range.
    std::vector<EventRecord> GetByTimeRange(const std::string &start,
                                            const std::string &end);

    // Get all records, most recent first.
    std::vector<EventRecord> GetAll(int limit = 1000);
};

}  // namespace cortexlink
