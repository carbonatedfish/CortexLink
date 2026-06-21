#include "mqtt/mqtt_client.h"

#include <spdlog/spdlog.h>

namespace cortexlink {

// ===========================================================================
// MqttSubscription
// ===========================================================================

MqttSubscription::MqttSubscription(const std::string &topic, int qos,
                                   MessageCallback callback)
    : topic_(topic), qos_(qos), callback_(std::move(callback))
{
}

void MqttSubscription::OnMessage(const std::string &topic,
                                 const std::string &payload) const
{
    if (callback_) {
        callback_(topic, payload);
    }
}

// ===========================================================================
// MqttClient
// ===========================================================================

MqttClient::MqttClient(const std::string &client_id, bool clean_session)
    : mosqpp::mosquittopp(client_id.c_str(), clean_session),
      client_id_(client_id),
      clean_session_(clean_session)
{
}

MqttClient::~MqttClient()
{
    if (connected_) {
        Disconnect();
    }
    // mosquittopp destructor handles libmosquitto cleanup
}

// ---- connection lifecycle -----------------------------------------------

bool MqttClient::Connect(const std::string &host, int port, int keepalive)
{
    if (!username_.empty()) {
        int rc = username_pw_set(username_.c_str(), password_.c_str());
        if (rc != MOSQ_ERR_SUCCESS) {
            spdlog::error("MqttClient: username_pw_set failed, rc={}", rc);
            return false;
        }
    }

    int rc = connect(host.c_str(), port, keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        spdlog::error("MqttClient: connect to {}:{} failed, rc={}", host, port, rc);
        return false;
    }

    spdlog::info("MqttClient: connecting to {}:{} (client_id={})", host, port, client_id_);
    return true;
}

void MqttClient::Disconnect()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_.clear();
        pending_subscriptions_.clear();
    }
    disconnect();
    connected_ = false;
    spdlog::info("MqttClient: disconnected (client_id={})", client_id_);
}

void MqttClient::SetCredentials(const std::string &username,
                                const std::string &password)
{
    username_ = username;
    password_ = password;
}

// ---- subscription management --------------------------------------------

bool MqttClient::Subscribe(MqttSubscription *sub)
{
    if (!sub) {
        return false;
    }

    int mid = 0;
    int rc = subscribe(&mid, sub->GetTopic().c_str(), sub->GetQos());
    if (rc != MOSQ_ERR_SUCCESS) {
        spdlog::error("MqttClient: subscribe to '{}' failed, rc={}",
                      sub->GetTopic(), rc);
        return false;
    }

    sub->SetMid(mid);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_subscriptions_[mid] = sub;
    }

    spdlog::info("MqttClient: subscribing to '{}' (mid={}, qos={})",
                 sub->GetTopic(), mid, sub->GetQos());
    return true;
}

bool MqttClient::Unsubscribe(MqttSubscription *sub)
{
    if (!sub) {
        return false;
    }

    int mid = 0;
    int rc = unsubscribe(&mid, sub->GetTopic().c_str());
    if (rc != MOSQ_ERR_SUCCESS) {
        spdlog::error("MqttClient: unsubscribe from '{}' failed, rc={}",
                      sub->GetTopic(), rc);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_.erase(sub->GetTopic());
    }

    spdlog::info("MqttClient: unsubscribing from '{}' (mid={})",
                 sub->GetTopic(), mid);
    return true;
}

// ---- publish ------------------------------------------------------------

bool MqttClient::PublishMessage(const std::string &topic,
                                const std::string &payload, int qos,
                                bool retain)
{
    int mid = 0;
    int rc = publish(&mid, topic.c_str(),
                     static_cast<int>(payload.size()), payload.data(),
                     qos, retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        spdlog::error("MqttClient: publish to '{}' failed, rc={}", topic, rc);
        return false;
    }
    spdlog::debug("MqttClient: published to '{}' (mid={}, len={}, qos={})",
                  topic, mid, payload.size(), qos);
    return true;
}

// ---- loop control -------------------------------------------------------

void MqttClient::LoopForever()
{
    spdlog::info("MqttClient: starting loop_forever (client_id={})", client_id_);
    loop_forever();
}

bool MqttClient::LoopStart()
{
    int rc = loop_start();
    if (rc != MOSQ_ERR_SUCCESS) {
        spdlog::error("MqttClient: loop_start failed, rc={}", rc);
        return false;
    }
    spdlog::info("MqttClient: loop_start ok (client_id={})", client_id_);
    return true;
}

void MqttClient::LoopStop()
{
    loop_stop(true);  // force stop
    spdlog::info("MqttClient: loop_stop (client_id={})", client_id_);
}

// ---- mosquittopp callbacks ----------------------------------------------

void MqttClient::on_connect(int rc)
{
    if (rc == MOSQ_ERR_SUCCESS) {
        connected_ = true;
        spdlog::info("MqttClient: on_connect success (client_id={})", client_id_);

        // Re-subscribe existing subscriptions (for persistent sessions)
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &kv : subscriptions_) {
            int mid = 0;
            subscribe(&mid, kv.second->GetTopic().c_str(), kv.second->GetQos());
        }
    } else {
        spdlog::error("MqttClient: on_connect failed, rc={}", rc);
    }
}

void MqttClient::on_disconnect(int rc)
{
    connected_ = false;
    if (rc == 0) {
        spdlog::info("MqttClient: on_disconnect (clean, client_id={})", client_id_);
    } else {
        spdlog::warn("MqttClient: on_disconnect unexpected, rc={}, client_id={}",
                     rc, client_id_);
    }
}

void MqttClient::on_message(const struct mosquitto_message *message)
{
    if (!message) {
        return;
    }

    std::string topic(message->topic);
    std::string payload(static_cast<const char *>(message->payload),
                        message->payloadlen);

    spdlog::debug("MqttClient: message topic='{}' payload_len={}",
                  topic, message->payloadlen);

    // Dispatch to the first matching subscription
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &kv : subscriptions_) {
        if (TopicMatches(kv.first, topic)) {
            kv.second->OnMessage(topic, payload);
            return;  // first-match wins
        }
    }

    spdlog::debug("MqttClient: no subscription matched topic='{}'", topic);
}

void MqttClient::on_subscribe(int mid, int qos_count, const int *granted_qos)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pending_subscriptions_.find(mid);
    if (it == pending_subscriptions_.end()) {
        // Re-subscription after reconnect — already in subscriptions_
        spdlog::debug("MqttClient: on_subscribe mid={} (re-subscription)", mid);
        return;
    }

    MqttSubscription *sub = it->second;
    pending_subscriptions_.erase(it);
    subscriptions_[sub->GetTopic()] = sub;

    spdlog::info("MqttClient: on_subscribe confirmed for '{}' (mid={}, qos={})",
                 sub->GetTopic(), mid,
                 (qos_count > 0) ? granted_qos[0] : -1);
}

void MqttClient::on_unsubscribe(int mid)
{
    spdlog::info("MqttClient: on_unsubscribe confirmed (mid={})", mid);
}

void MqttClient::on_error()
{
    spdlog::error("MqttClient: on_error (client_id={})", client_id_);
}

// ---- topic matching helpers ---------------------------------------------

// MQTT topic matching: '+' matches one level, '#' matches zero or more.
std::vector<std::string> MqttClient::SplitTopic(const std::string &topic)
{
    std::vector<std::string> parts;
    if (topic.empty()) {
        return parts;
    }

    std::string::size_type start = 0;
    std::string::size_type end = 0;

    while ((end = topic.find('/', start)) != std::string::npos) {
        parts.push_back(topic.substr(start, end - start));
        start = end + 1;
    }
    parts.push_back(topic.substr(start));

    return parts;
}

bool MqttClient::TopicMatches(const std::string &subscription,
                              const std::string &topic)
{
    // Fast path: exact match
    if (subscription == topic) {
        return true;
    }

    // Fast path: '#' matches everything
    if (subscription == "#") {
        return true;
    }

    std::vector<std::string> sub_parts = SplitTopic(subscription);
    std::vector<std::string> topic_parts = SplitTopic(topic);

    size_t si = 0;
    size_t ti = 0;

    while (si < sub_parts.size() && ti < topic_parts.size()) {
        const std::string &sp = sub_parts[si];

        if (sp == "#") {
            // Multi-level wildcard — matches everything that remains,
            // including zero additional levels. Must be the last part.
            return (si == sub_parts.size() - 1);
        }

        if (sp == "+") {
            // Single-level wildcard — matches exactly one topic level
            si++;
            ti++;
            continue;
        }

        // Exact level match required
        if (sp != topic_parts[ti]) {
            return false;
        }

        si++;
        ti++;
    }

    // Both exhausted → exact match
    if (si == sub_parts.size() && ti == topic_parts.size()) {
        return true;
    }

    // Subscription ends with '#' which matches zero remaining topic levels
    if (si == sub_parts.size() - 1 && sub_parts[si] == "#") {
        return true;
    }

    return false;
}

}  // namespace cortexlink
