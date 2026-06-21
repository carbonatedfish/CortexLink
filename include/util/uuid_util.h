#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace cortexlink {
namespace util {

// Convert a standard UUID string (with hyphens) to a 16-byte BLOB.
// e.g. "550e8400-e29b-41d4-a716-446655440000" → std::array<uint8_t, 16>
// Returns std::nullopt on malformed input.
std::array<uint8_t, 16> UuidToBlob(const std::string &uuid_str);

// Convert a 16-byte BLOB to a standard UUID string with hyphens.
// e.g. {0x55,0x0e,...} → "550e8400-e29b-41d4-a716-446655440000"
std::string BlobToUuid(const std::array<uint8_t, 16> &blob);

// Convert a 16-byte BLOB (as raw pointer) to a standard UUID string.
std::string BlobToUuid(const uint8_t *blob);

// Generate a UUID-format string seeded from the current UNIX timestamp.
// Format: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
// Thread-safe and suitable for MQTT msg_id generation.
std::string GenerateUuid();

}  // namespace util
}  // namespace cortexlink
