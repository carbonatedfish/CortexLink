#include "util/sql_util.h"

#include <string>

#include "util/uuid_util.h"

namespace cortexlink {
namespace util {

nlohmann::json RowToJson(sqlite3_stmt *stmt)
{
    int col_count = sqlite3_column_count(stmt);
    nlohmann::json obj;

    for (int i = 0; i < col_count; ++i) {
        const char *name = sqlite3_column_name(stmt, i);
        std::string col_name = name ? name : "";

        int type = sqlite3_column_type(stmt, i);
        switch (type) {
        case SQLITE_INTEGER:
            obj[col_name] = sqlite3_column_int64(stmt, i);
            break;

        case SQLITE_FLOAT:
            obj[col_name] = sqlite3_column_double(stmt, i);
            break;

        case SQLITE_TEXT: {
            const char *text = reinterpret_cast<const char *>(
                sqlite3_column_text(stmt, i));
            int len = sqlite3_column_bytes(stmt, i);
            obj[col_name] = text ? std::string(text, static_cast<size_t>(len))
                                 : "";
            break;
        }

        case SQLITE_BLOB: {
            const void *blob = sqlite3_column_blob(stmt, i);
            int len = sqlite3_column_bytes(stmt, i);
            if (blob && len == 16) {
                // 16-byte BLOB → UUID string (e.g. dev_id, evt_id, user_id)
                obj[col_name] = util::BlobToUuid(
                    static_cast<const uint8_t *>(blob));
            } else if (blob && len > 0) {
                // Other BLOB → hex string
                static const char hex[] = "0123456789abcdef";
                std::string hex_str;
                hex_str.reserve(static_cast<size_t>(len) * 2);
                const auto *bytes = static_cast<const uint8_t *>(blob);
                for (int j = 0; j < len; ++j) {
                    hex_str += hex[(bytes[j] >> 4) & 0xF];
                    hex_str += hex[bytes[j] & 0xF];
                }
                obj[col_name] = std::move(hex_str);
            } else {
                obj[col_name] = "";
            }
            break;
        }

        case SQLITE_NULL:
        default:
            obj[col_name] = nullptr;
            break;
        }
    }

    return obj;
}

}  // namespace util
}  // namespace cortexlink
