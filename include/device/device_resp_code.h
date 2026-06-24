#pragma once

namespace cortexlink {

// Response codes for host-to-device MQTT replies on device/<uuid>/resp/m2s.
//
// Value ranges:
//   0        Success
//   1-19     General errors
//   20-39    Action-specific errors
//   99       Catch-all unknown error
enum class DeviceRespCode : int {
    // Success
    OK = 0,

    // General errors (1-19)
    DEVICE_NOT_FOUND  = 1,   // Device UUID not registered in database
    DEVICE_OFFLINE    = 2,   // Device is currently offline
    INTERNAL_ERROR    = 3,   // Host-side internal processing failure
    INVALID_REQUEST   = 4,   // Request JSON parse failure
    MISSING_FIELD     = 5,   // Required field missing in request
    TIMEOUT           = 6,   // Operation timed out, no device response

    // Action errors (20-39)
    ACTION_NOT_FOUND  = 20,  // Action ID not recognized by device
    INVALID_PARAMS    = 21,  // Parameter validation failed
    ACTION_FAILED     = 22,  // Device reported action execution failure
    DEVICE_BUSY       = 23,  // Device busy, retry later

    // Catch-all
    UNKNOWN_ERROR     = 99,  // Unclassified error
};

}  // namespace cortexlink
