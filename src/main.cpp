#include <cstdlib>
#include <string>

#include <spdlog/spdlog.h>

#include "db/db_table.h"
#include "util/uuid_util.h"
#include "db/device_data_table.h"
#include "db/device_property_table.h"
#include "db/event_record_table.h"
#include "db/event_rule_table.h"
#include "db/event_table.h"
#include "db/rule_table.h"
#include "db/user_profile_table.h"

using namespace cortexlink;

int main(int argc, char **argv)
{
    // Resolve database path: ~/.cortexlink/cortexlink.db
    const char *home = std::getenv("HOME");
    if (!home) {
        spdlog::error("HOME environment variable not set");
        return 1;
    }
    std::string db_path = std::string(home) + "/.cortexlink/cortexlink.db";

    // Initialize the database
    if (!DBTable::Initialize(db_path)) {
        spdlog::error("Failed to initialize database at {}", db_path);
        return 1;
    }

    // Create all tables
    DevicePropertyTable dev_prop_table;
    EventTable event_table;
    RuleTable rule_table;
    DeviceDataTable dev_data_table;
    UserProfileTable user_table;
    EventRuleTable evt_rule_table;
    EventRecordTable evt_record_table;

    bool all_ok = true;
    all_ok = dev_prop_table.CreateTable() && all_ok;
    all_ok = event_table.CreateTable() && all_ok;
    all_ok = rule_table.CreateTable() && all_ok;
    all_ok = dev_data_table.CreateTable() && all_ok;
    all_ok = user_table.CreateTable() && all_ok;
    all_ok = evt_rule_table.CreateTable() && all_ok;
    all_ok = evt_record_table.CreateTable() && all_ok;

    if (!all_ok) {
        spdlog::error("Failed to create one or more tables");
        DBTable::Shutdown();
        return 1;
    }

    spdlog::info("All tables created successfully at {}", db_path);

    // Quick smoke test: insert and query a device
    DevicePropertyTable::DeviceProperty dev{};
    dev.dev_name = "Test Sensor";
    dev.dev_type = "sensor";
    dev.dev_subtype = "temperature";
    dev.dev_state = "online";
    dev.location = "Lab";
    dev.user_param = "test device";
    dev.actions = R"({"actions":[]})";
    dev.events = R"({"evt_id":[]})";
    dev.data = R"({"data":[]})";
    // Use a fixed test UUID
    std::string test_uuid = "a1b2c3d4-e5f6-4789-ab12-cd34ef567890";
    auto blob = util::UuidToBlob(test_uuid);
    dev.dev_id = blob;

    if (dev_prop_table.Upsert(dev)) {
        spdlog::info("Test device inserted with uuid={}", test_uuid);
    }

    auto result = dev_prop_table.GetByDevId(blob);
    if (result.has_value()) {
        spdlog::info("Test device queried: name={}, state={}",
                     result->dev_name, result->dev_state);
    }

    // Cleanup
    DBTable::Shutdown();
    spdlog::info("Database layer smoke test complete");
    return 0;
}
