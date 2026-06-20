#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db/db_table.h"

namespace cortexlink {

class EventTable : public DBTable {
public:
    struct Event {
        std::array<uint8_t, 16> evt_id;
        std::array<uint8_t, 16> dev_id;
        std::string evt_name;
        std::string desc;
        std::string params;  // JSON string
    };

    bool CreateTable() override;

    bool Insert(const Event &evt);
    bool Delete(const std::array<uint8_t, 16> &evt_id);

    std::optional<Event> GetByEvtId(const std::array<uint8_t, 16> &evt_id);
    std::vector<Event> GetByDevId(const std::array<uint8_t, 16> &dev_id);
    std::vector<Event> GetAll();
};

}  // namespace cortexlink
