#include "llm/open_claw_client.h"

#include <spdlog/spdlog.h>

#include <httplib/httplib.h>

namespace cortexlink {

// ============================================================================
// Construction / Destruction
// ============================================================================

OpenClawClient::OpenClawClient()
{
    client_ = std::make_unique<httplib::Client>(endpoint_);

    // 2 s to establish the TCP connection, 5 s to receive the response.
    client_->set_connection_timeout(2, 0);
    client_->set_read_timeout(5, 0);

    // Every request sends the same Content-Type.
    client_->set_default_headers({
        {"Content-Type", "application/json"}
    });

    spdlog::info("OpenClawClient: created (endpoint={})", endpoint_);
}

OpenClawClient::~OpenClawClient()
{
    spdlog::info("OpenClawClient: destroyed");
}

// ============================================================================
// Configuration
// ============================================================================

void OpenClawClient::SetEndpoint(const std::string &endpoint)
{
    endpoint_ = endpoint;
    client_ = std::make_unique<httplib::Client>(endpoint_);

    client_->set_connection_timeout(2, 0);
    client_->set_read_timeout(5, 0);
    client_->set_default_headers({
        {"Content-Type", "application/json"}
    });
}

// ============================================================================
// Send Message
// ============================================================================

bool OpenClawClient::SendMessage(const std::string &session,
                                  const std::string &content)
{
    if (!client_) {
        spdlog::warn("OpenClawClient: client is null");
        return false;
    }

    nlohmann::json body = BuildRequestBody(session, content);
    std::string body_str = body.dump();

    spdlog::debug("OpenClawClient: POST {} body={}", api_path_, body_str);

    auto res = client_->Post(api_path_, body_str, "application/json");

    if (!res) {
        spdlog::warn("OpenClawClient: POST failed (error={})",
                      static_cast<int>(res.error()));
        return false;
    }

    if (res->status < 200 || res->status >= 300) {
        spdlog::warn("OpenClawClient: POST returned status {} body={}",
                      res->status, res->body);
        return false;
    }

    spdlog::info("OpenClawClient: message sent (session='{}', content_len={})",
                 session, content.size());
    return true;
}

// ============================================================================
// Send Message And Get Response
// ============================================================================

std::optional<nlohmann::json>
OpenClawClient::SendMessageAndGetResponse(const std::string &session,
                                           const std::string &content)
{
    if (!client_) {
        spdlog::warn("OpenClawClient: client is null in SendMessageAndGetResponse");
        return std::nullopt;
    }

    nlohmann::json body = BuildRequestBody(session, content);
    std::string body_str = body.dump();

    spdlog::debug("OpenClawClient: POST {} body={}", api_path_, body_str);

    auto res = client_->Post(api_path_, body_str, "application/json");

    if (!res) {
        spdlog::warn("OpenClawClient: POST failed (error={})",
                      static_cast<int>(res.error()));
        return std::nullopt;
    }

    if (res->status < 200 || res->status >= 300) {
        spdlog::warn("OpenClawClient: POST returned status {} body={}",
                      res->status, res->body);
        return std::nullopt;
    }

    spdlog::info("OpenClawClient: message sent and response received "
                 "(session='{}', content_len={})", session, content.size());

    try {
        auto parsed = nlohmann::json::parse(res->body);
        spdlog::debug("OpenClawClient: response parsed successfully session='{}'", session);
        return parsed;
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::warn("OpenClawClient: failed to parse POST response JSON: {}",
                      e.what());
        return std::nullopt;
    }
}

// ============================================================================
// Get History
// ============================================================================

std::optional<nlohmann::json>
OpenClawClient::GetHistory(const std::string &session)
{
    if (!client_) {
        spdlog::warn("OpenClawClient: client is null in GetHistory");
        return std::nullopt;
    }

    std::string path = history_api_path_ + "?session=" + session;

    spdlog::debug("OpenClawClient: GET {}", path);

    auto res = client_->Get(path);

    if (!res) {
        spdlog::warn("OpenClawClient: GET failed (error={})",
                      static_cast<int>(res.error()));
        return std::nullopt;
    }

    if (res->status < 200 || res->status >= 300) {
        spdlog::warn("OpenClawClient: GET returned status {} body={}",
                      res->status, res->body);
        return std::nullopt;
    }

    try {
        auto parsed = nlohmann::json::parse(res->body);
        spdlog::debug("OpenClawClient: history fetched session='{}'", session);
        return parsed;
    } catch (const nlohmann::json::parse_error &e) {
        spdlog::warn("OpenClawClient: failed to parse history response JSON: {}",
                      e.what());
        return std::nullopt;
    }
}

// ============================================================================
// Helpers
// ============================================================================

nlohmann::json OpenClawClient::BuildRequestBody(const std::string &session,
                                                 const std::string &content)
{
    nlohmann::json j;
    j["session"] = session;
    j["content"] = content;
    return j;
}

}  // namespace cortexlink
