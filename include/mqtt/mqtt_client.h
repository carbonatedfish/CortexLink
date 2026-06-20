#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <mosquittopp.h>

// Remote broker address — modify this macro to point at the target broker
#ifndef MQTT_BROKER_ADDRESS
#define MQTT_BROKER_ADDRESS "localhost"
#endif

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif

namespace cortexlink {

// One subscription = one object. Each MqttSubscription binds a topic,
// a QoS level, and a message callback together. Hand it to MqttClient
// to register it with the broker.
class MqttSubscription {
public:
    using MessageCallback = std::function<void(const std::string &topic,
                                               const std::string &payload)>;

    MqttSubscription(const std::string &topic, int qos,
                     MessageCallback callback);
    ~MqttSubscription() = default;

    const std::string &GetTopic() const { return topic_; }
    int GetQos() const { return qos_; }
    int GetMid() const { return mid_; }
    void SetMid(int mid) { mid_ = mid; }

    // Called by MqttClient when a matching message arrives
    void OnMessage(const std::string &topic, const std::string &payload) const;

private:
    std::string topic_;
    int qos_;
    int mid_ = -1;
    MessageCallback callback_;
};

// MqttClient wraps a single mosquittopp connection. It owns the
// physical MQTT link to the broker and dispatches incoming messages
// to the appropriate MqttSubscription objects.
class MqttClient : public mosqpp::mosquittopp {
public:
    explicit MqttClient(const std::string &client_id,
                        bool clean_session = true);
    ~MqttClient() override;

    // ---- connection lifecycle -------------------------------------------
    bool Connect(const std::string &host = MQTT_BROKER_ADDRESS,
                 int port = MQTT_BROKER_PORT,
                 int keepalive = 60);
    void Disconnect();
    bool IsConnected() const { return connected_; }

    // ---- credentials ----------------------------------------------------
    void SetCredentials(const std::string &username,
                        const std::string &password);

    // ---- subscription management (thread-safe) --------------------------
    bool Subscribe(MqttSubscription *sub);
    bool Unsubscribe(MqttSubscription *sub);

    // ---- publish -------------------------------------------------------
    bool PublishMessage(const std::string &topic, const std::string &payload,
                        int qos = 0, bool retain = false);

    // ---- loop control ---------------------------------------------------
    void LoopForever();
    bool LoopStart();
    void LoopStop();

private:
    // mosquittopp callbacks
    void on_connect(int rc) override;
    void on_disconnect(int rc) override;
    void on_message(const struct mosquitto_message *message) override;
    void on_subscribe(int mid, int qos_count, const int *granted_qos) override;
    void on_unsubscribe(int mid) override;
    void on_error() override;

    // Helpers
    static bool TopicMatches(const std::string &subscription,
                             const std::string &topic);
    static std::vector<std::string> SplitTopic(const std::string &topic);

    std::string client_id_;
    bool clean_session_;
    bool connected_ = false;
    std::string username_;
    std::string password_;

    // topic → subscription (confirmed subscriptions only)
    std::unordered_map<std::string, MqttSubscription *> subscriptions_;
    // mid → subscription (pending, not yet confirmed by broker)
    std::unordered_map<int, MqttSubscription *> pending_subscriptions_;
    mutable std::mutex mutex_;
};

// One publisher = one topic. Each MqttPublisher is bound to a single
// topic and publishes every message to that topic through the shared
// MqttClient connection.
class MqttPublisher {
public:
    MqttPublisher(MqttClient *client, const std::string &topic,
                  int qos = 0, bool retain = false);
    ~MqttPublisher() = default;

    // Publish a string payload to the bound topic.
    bool Publish(const std::string &payload);
    // Publish a binary payload to the bound topic.
    bool Publish(const void *payload, size_t len);

    const std::string &GetTopic() const { return topic_; }
    int GetQos() const { return qos_; }
    bool GetRetain() const { return retain_; }
    void SetQos(int qos) { qos_ = qos; }
    void SetRetain(bool retain) { retain_ = retain; }

private:
    MqttClient *client_;
    std::string topic_;
    int qos_;
    bool retain_;
};

}  // namespace cortexlink
