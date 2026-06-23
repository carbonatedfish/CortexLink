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
