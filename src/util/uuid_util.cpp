#include "util/uuid_util.h"

#include <chrono>
#include <cstdio>
#include <random>

#include <spdlog/spdlog.h>

namespace cortexlink {
namespace util {

std::array<uint8_t, 16> UuidToBlob(const std::string &uuid_str)
{
    std::array<uint8_t, 16> blob{};
    // Expected format: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars)
    if (uuid_str.size() != 36) {
        spdlog::debug("UUID parse failed: wrong length {} (expected 36): '{}'",
                      uuid_str.size(), uuid_str);
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
        spdlog::debug("UUID parse failed: sscanf parsed {} fields (expected 16) for '{}'",
                      n, uuid_str);
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

std::string GenerateUuid()
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto ts = duration_cast<microseconds>(now.time_since_epoch()).count();

    // Use a thread_local random engine seeded from system entropy,
    // so successive calls from the same thread at the same microsecond
    // produce distinct UUIDs.
    thread_local std::mt19937_64 rng(std::random_device{}());
    uint64_t rand_val = rng();

    // UUID v1-style layout: timestamp (60 bits) + clock_seq (14 bits) + node (48 bits)
    uint32_t time_low = static_cast<uint32_t>(ts & 0xFFFFFFFF);
    uint16_t time_mid = static_cast<uint16_t>((ts >> 32) & 0xFFFF);
    uint16_t time_hi = static_cast<uint16_t>(((ts >> 48) & 0x0FFF) | 0x1000);
    uint16_t clock_seq = static_cast<uint16_t>((rand_val & 0x3FFF) | 0x8000);
    uint16_t node_hi = static_cast<uint16_t>((rand_val >> 16) & 0xFFFF);
    uint32_t node_lo = static_cast<uint32_t>(rand_val >> 32);

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%04x%08x",
                  time_low, time_mid, time_hi,
                  clock_seq, node_hi, node_lo);
    return std::string(buf);
}

}  // namespace util
}  // namespace cortexlink
