#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db/db_table.h"

namespace cortexlink {

class DevicePropertyTable : public DBTable {
public:
    struct DeviceProperty {
        std::array<uint8_t, 16> dev_id;
        std::string dev_name;
        std::string dev_type;       // sensor / actuator / composite
        std::string dev_subtype;
        std::string dev_state;      // online / offline
        std::string location;
        std::string user_param;
        std::string actions;        // JSON string
        std::string events;         // JSON string
        std::string data;           // JSON string
    };

    bool CreateTable() override;

    // Insert a new device. Returns false if dev_id already exists.
    bool Insert(const DeviceProperty &dev);

    // Update an existing device (all fields except dev_id).
    bool Update(const DeviceProperty &dev);

    // Insert or replace (used for broadcast/online).
    bool Upsert(const DeviceProperty &dev);

    // Delete a device by UUID BLOB.
    bool Delete(const std::array<uint8_t, 16> &dev_id);

    // Query a single device by UUID BLOB.
    std::optional<DeviceProperty> GetByDevId(
        const std::array<uint8_t, 16> &dev_id);

    // Query all devices.
    std::vector<DeviceProperty> GetAll();

    // Update only the dev_state field.
    bool UpdateState(const std::array<uint8_t, 16> &dev_id,
                     const std::string &state);
};

}  // namespace cortexlink
