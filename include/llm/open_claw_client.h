#pragma once

#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

// Forward declaration — only the .cpp needs the full httplib.h.
namespace httplib {
class Client;
}  // namespace httplib

namespace cortexlink {

// OpenClawClient sends HTTP POST messages to the OpenClaw service.
//
// Lifecycle:
//   Construct → ready to use → Destruct
//
// Protocol:
//   POST /api/chat/send
//   Body: {"session":"...", "content":"..."}
//   Content-Type: application/json
//
// SendMessage() is synchronous and blocks until a response or timeout.
// On failure (connection refused, timeout, non-2xx status),
// logs a warning and returns false.  The caller is expected to handle
// retries if needed.
class OpenClawClient {
public:
    OpenClawClient();
    ~OpenClawClient();

    OpenClawClient(const OpenClawClient &) = delete;
    OpenClawClient &operator=(const OpenClawClient &) = delete;

    // Override the default endpoint.  Must be called before the first
    // SendMessage() — typically right after construction.
    // Example: "http://127.0.0.1:18789"
    void SetEndpoint(const std::string &endpoint);

    // Send a message to OpenClaw.
    // @param session  Opaque session identifier (can be empty).
    // @param content  The message content to send.
    // @return true if the request was accepted (HTTP 2xx).
    bool SendMessage(const std::string &session,
                     const std::string &content);

    // Send a message and return the parsed JSON response body.
    // Returns std::nullopt on connection failure, non-2xx status,
    // or non-JSON body.
    std::optional<nlohmann::json> SendMessageAndGetResponse(
        const std::string &session, const std::string &content);

    // Fetch the chat history for the given session.
    // GET /api/chat/history?session=<session>
    // Returns std::nullopt on connection failure, non-2xx status,
    // or non-JSON body.
    std::optional<nlohmann::json> GetHistory(const std::string &session);

private:
    // Build the JSON body: {"session":"...","content":"..."}
    static nlohmann::json BuildRequestBody(const std::string &session,
                                           const std::string &content);

    std::string endpoint_ = "http://127.0.0.1:18789";
    std::string api_path_ = "/api/chat/send";
    std::string history_api_path_ = "/api/chat/history";
    std::unique_ptr<httplib::Client> client_;
};

}  // namespace cortexlink
