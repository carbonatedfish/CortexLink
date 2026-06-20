#pragma once

#include <optional>
#include <string>
#include <vector>

#include "db/db_table.h"

namespace cortexlink {

class UserProfileTable : public DBTable {
public:
    struct UserProfile {
        std::array<uint8_t, 16> user_id;
        std::string user_name;
        std::string preference;  // JSON string
    };

    bool CreateTable() override;

    bool Insert(const UserProfile &user);
    bool Update(const UserProfile &user);
    bool Delete(const std::array<uint8_t, 16> &user_id);

    std::optional<UserProfile> GetByUserId(
        const std::array<uint8_t, 16> &user_id);
    std::vector<UserProfile> GetAll();
};

}  // namespace cortexlink
