#include "util/uuid_util.h"

#include <cstdio>

namespace cortexlink {
namespace util {

std::array<uint8_t, 16> UuidToBlob(const std::string &uuid_str)
{
    std::array<uint8_t, 16> blob{};
    // Expected format: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars)
    if (uuid_str.size() != 36) {
        return blob;
    }

    unsigned int bytes[16];
    int n = std::sscanf(uuid_str.c_str(),
                         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                         &bytes[0], &bytes[1], &bytes[2], &bytes[3],
                         &bytes[4], &bytes[5], &bytes[6], &bytes[7],
                         &bytes[8], &bytes[9], &bytes[10], &bytes[11],
                         &bytes[12], &bytes[13], &bytes[14], &bytes[15]);
    if (n != 16) {
        return blob;
    }

    for (int i = 0; i < 16; ++i) {
        blob[i] = static_cast<uint8_t>(bytes[i]);
    }
    return blob;
}

std::string BlobToUuid(const std::array<uint8_t, 16> &blob)
{
    return BlobToUuid(blob.data());
}

std::string BlobToUuid(const uint8_t *blob)
{
    if (!blob) {
        return {};
    }

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  blob[0], blob[1], blob[2], blob[3],
                  blob[4], blob[5], blob[6], blob[7],
                  blob[8], blob[9], blob[10], blob[11],
                  blob[12], blob[13], blob[14], blob[15]);
    return buf;
}

}  // namespace util
}  // namespace cortexlink
