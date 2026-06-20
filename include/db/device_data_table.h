#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db/db_table.h"

namespace cortexlink {

class DeviceDataTable : public DBTable {
public:
    struct DeviceData {
        std::array<uint8_t, 16> dev_id;
        std::string data_name;
        std::string data_type;  // int / float / str
        std::string data_val;
    };

    bool CreateTable() override;

    // Insert or replace data for a device.
    bool Upsert(const DeviceData &data);

    // Get a single data value.
    std::optional<DeviceData> Get(const std::array<uint8_t, 16> &dev_id,
                                  const std::string &data_name);

    // Get all data for a device.
    std::vector<DeviceData> GetByDevId(const std::array<uint8_t, 16> &dev_id);

    bool Delete(const std::array<uint8_t, 16> &dev_id,
                const std::string &data_name);
};

}  // namespace cortexlink
