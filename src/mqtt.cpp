#include "mqtt.h"
#include "config.h"
#include "util.h"
#include <mosquitto.h>
#include <atomic>
#include <ctime>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string_view>

namespace {

constexpr int max_publish_size = 1024 * 1024;
constexpr const char* online = R"({"state":"online"})";
constexpr const char* offline = R"({"state":"offline"})";

struct MqttSession {
    mosquitto* client = nullptr;
    std::string prefix;
    std::string sensor;
    std::atomic<bool> connected{false};
    std::atomic<bool> stopping{false};
    // Owned by the mosquitto callback thread.
    int command_mid = 0;
    int sensor_mid = 0;
    bool subscriptions_failed = false;
};

std::mutex lifecycle_mutex;
std::mutex publish_mutex;
MqttSession* active_session = nullptr;

std::string json_quote_content(std::string_view input) {
    std::string result;
    for (unsigned char ch : input) {
        if (ch == '"' || ch == '\\') result.push_back('\\');
        if (ch < 32) result += std::format("\\u{:04x}", ch);
        else result.push_back(static_cast<char>(ch));
    }
    return result;
}

void on_connect(mosquitto* client, void* context, int result) noexcept {
    auto& session = *static_cast<MqttSession*>(context);
    if (session.stopping.load()) return;
    try {
        if (result != 0) {
            g_logger.warn("MQTT: Broker refused connection ({})", mosquitto_connack_string(result));
            trigger_error(701);
            return;
        }
        session.connected.store(false);
        session.command_mid = session.sensor_mid = 0;
        const auto status = session.prefix + "/status";
        mosquitto_publish(client, nullptr, status.c_str(), static_cast<int>(std::strlen(offline)), offline, 0, true);
        const auto commands = session.prefix + "/command/#";
        session.subscriptions_failed =
            mosquitto_subscribe(client, &session.command_mid, commands.c_str(), 0) != MOSQ_ERR_SUCCESS ||
            (!session.sensor.empty() &&
             mosquitto_subscribe(client, &session.sensor_mid, session.sensor.c_str(), 0) != MOSQ_ERR_SUCCESS);
        if (session.subscriptions_failed) trigger_error(701);
    } catch (...) {
        session.subscriptions_failed = true;
    }
}

void on_subscribe(mosquitto*, void* context, int mid, int qos_count, const int* granted_qos) noexcept {
    auto& session = *static_cast<MqttSession*>(context);
    if (session.stopping.load() || session.subscriptions_failed || mid == 0 ||
        (mid != session.command_mid && mid != session.sensor_mid)) return;
    try {
        if (qos_count != 1 || !granted_qos || granted_qos[0] >= 0x80) {
            session.subscriptions_failed = true;
            g_logger.warn("MQTT: A required subscription was rejected by the broker");
            trigger_error(701);
            return;
        }
        if (mid == session.command_mid) session.command_mid = 0;
        else session.sensor_mid = 0;
        if (session.command_mid || session.sensor_mid) return;
        session.connected.store(true);
        clear_error(701);
        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
        mqtt_publish(session.prefix + "/status", online, true);
        mqtt_publish(session.prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);
        publish_ha_discovery();
    } catch (...) {
        session.connected.store(false);
    }
}

void on_disconnect(mosquitto*, void* context, int result) noexcept {
    auto& session = *static_cast<MqttSession*>(context);
    session.connected.store(false);
    if (!session.stopping.load() && result != 0) {
        try { trigger_error(701); } catch (...) {}
    }
}

void on_message(mosquitto*, void* context, const mosquitto_message* message) noexcept {
    auto& session = *static_cast<MqttSession*>(context);
    if (session.stopping.load() || !message->topic || !message->payload ||
        message->payloadlen <= 0 || message->payloadlen > 4096) return;
    try {
        const std::string_view topic(message->topic);
        const auto payload = trim({static_cast<const char*>(message->payload), static_cast<size_t>(message->payloadlen)});
        const auto wake = [&](bool power) {
            if (power) g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
            if (set_display_power(power)) {
                mqtt_publish(session.prefix + "/status/screen", power ? "ON" : "OFF", true);
            }
        };
        const bool on = payload == "ON" || payload == "1" || payload == "true";
        if (!session.sensor.empty() && topic == session.sensor) {
            if (on || payload == "motion") wake(true);
            return;
        }
        if (!topic.starts_with(session.prefix) || topic.size() <= session.prefix.size()) return;
        const auto command = topic.substr(session.prefix.size());
        if (message->retain && command != "/command/screen") return;
        const bool press = payload == "PRESS" || payload == "1" || payload == "true";
        if (command == "/command/next" && (press || on || payload == "next")) g_remote_command.store(1);
        else if (command == "/command/prev" && (press || payload == "prev")) g_remote_command.store(2);
        else if (command == "/command/pause" && (press || payload == "pause")) g_remote_command.store(3);
        else if (command == "/command/screen") {
            if (on) wake(true);
            else if (payload == "OFF" || payload == "0" || payload == "false") wake(false);
        }
    } catch (...) {}
}

} // namespace

void mqtt_publish(const std::string& topic, const std::string& payload, bool retain) {
    if (payload.size() > max_publish_size) return;
    std::lock_guard lock(publish_mutex);
    if (!active_session || !active_session->connected.load()) return;
    mosquitto_publish(active_session->client, nullptr, topic.c_str(),
                      static_cast<int>(payload.size()), payload.data(), 0, retain);
}

void publish_ha_discovery() {
    std::string prefix;
    { std::shared_lock<std::shared_mutex> lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
    
    prefix = json_quote_content(prefix);

    // Device configuration block
    std::string device_json = "\"device\":{"
        "\"identifiers\":[\"piTrove_frame\"],"
        "\"name\":\"piTrove Picture Frame\","
        "\"model\":\"Modular Frame\","
        "\"manufacturer\":\"UnDadFeated\""
        "}";

    // 1. Screen Switch
    std::string screen_switch_json = "{"
        "\"name\":\"Screen Switch\","
        "\"unique_id\":\"piTrove_screen_switch\","
        "\"state_topic\":\"" + prefix + "/status/screen\","
        "\"command_topic\":\"" + prefix + "/command/screen\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        + device_json +
        "}";
    mqtt_publish("homeassistant/switch/piTrove_screen/config", screen_switch_json, true);

    // 2. Skip Next Button
    std::string next_button_json = "{"
        "\"name\":\"Next Slide\","
        "\"unique_id\":\"piTrove_next_button\","
        "\"command_topic\":\"" + prefix + "/command/next\","
        "\"payload_press\":\"PRESS\","
        + device_json +
        "}";
    mqtt_publish("homeassistant/button/piTrove_next/config", next_button_json, true);

    // 3. Previous Slide Button
    std::string prev_button_json = "{"
        "\"name\":\"Previous Slide\","
        "\"unique_id\":\"piTrove_prev_button\","
        "\"command_topic\":\"" + prefix + "/command/prev\","
        "\"payload_press\":\"PRESS\","
        + device_json +
        "}";
    mqtt_publish("homeassistant/button/piTrove_prev/config", prev_button_json, true);

    // 4. Play/Pause Toggle Button
    std::string pause_button_json = "{"
        "\"name\":\"Play/Pause Toggle\","
        "\"unique_id\":\"piTrove_pause_button\","
        "\"command_topic\":\"" + prefix + "/command/pause\","
        "\"payload_press\":\"PRESS\","
        + device_json +
        "}";
    mqtt_publish("homeassistant/button/piTrove_pause/config", pause_button_json, true);

    // 5. Motion Binary Sensor (if configured)
    {
        std::string sensor_topic;
        { std::shared_lock<std::shared_mutex> lk(g_config_mtx); sensor_topic = g_cfg.mqtt_motionsensor_topic; }
        if (!sensor_topic.empty()) {
            std::string motion_sensor_json = "{"
                "\"name\":\"Motion Sensor\","
                "\"unique_id\":\"piTrove_motion_sensor\","
                "\"state_topic\":\"" + json_quote_content(sensor_topic) + "\","
                "\"payload_on\":\"ON\","
                "\"payload_off\":\"OFF\","
                "\"device_class\":\"motion\","
                + device_json +
                "}";
            mqtt_publish("homeassistant/binary_sensor/piTrove_motion/config", motion_sensor_json, true);
        }
    }
}

void start_mqtt_client() {
    std::lock_guard lifecycle(lifecycle_mutex);
    if (active_session || !g_running.load()) return;
    Config config;
    {
        std::shared_lock lock(g_config_mtx);
        if (!g_cfg.mqtt_enabled) return;
        config = g_cfg;
    }

    static const int library_status = mosquitto_lib_init();
    if (library_status != MOSQ_ERR_SUCCESS) {
        trigger_error(701);
        return;
    }
    auto session = std::make_unique<MqttSession>();
    session->prefix = config.mqtt_topic_prefix;
    session->sensor = config.mqtt_motionsensor_topic;
    std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)>
        client(mosquitto_new(nullptr, true, session.get()), mosquitto_destroy);
    if (!client) {
        trigger_error(701);
        return;
    }
    session->client = client.get();
    const auto status = session->prefix + "/status";
    if ((!config.mqtt_user.empty() &&
         mosquitto_username_pw_set(client.get(), config.mqtt_user.c_str(), config.mqtt_pass.c_str()) != MOSQ_ERR_SUCCESS) ||
        (config.mqtt_tls_enabled &&
         (mosquitto_tls_set(client.get(), config.mqtt_ca_cert.empty() ? "/etc/ssl/certs/ca-certificates.crt" :
                            config.mqtt_ca_cert.c_str(), nullptr, nullptr, nullptr, nullptr) != MOSQ_ERR_SUCCESS ||
          mosquitto_tls_opts_set(client.get(), 1, "tlsv1.2", nullptr) != MOSQ_ERR_SUCCESS)) ||
        mosquitto_will_set(client.get(), status.c_str(), static_cast<int>(std::strlen(offline)), offline, 0, true) != MOSQ_ERR_SUCCESS) {
        trigger_error(701);
        return;
    }

    mosquitto_connect_callback_set(client.get(), on_connect);
    mosquitto_disconnect_callback_set(client.get(), on_disconnect);
    mosquitto_message_callback_set(client.get(), on_message);
    mosquitto_subscribe_callback_set(client.get(), on_subscribe);
    mosquitto_reconnect_delay_set(client.get(), 2, 60, true);

    int rc = mosquitto_connect_async(client.get(), config.mqtt_broker.c_str(), config.mqtt_port, 30);
    if (rc == MOSQ_ERR_SUCCESS) {
        std::lock_guard publishing(publish_mutex);
        active_session = session.get();
        rc = mosquitto_loop_start(client.get());
        if (rc != MOSQ_ERR_SUCCESS) active_session = nullptr;
    }
    if (rc != MOSQ_ERR_SUCCESS) {
        g_logger.warn("MQTT: {}", mosquitto_strerror(rc));
        trigger_error(701);
        return;
    }
    client.release();
    session.release();
}

void stop_mqtt_client() {
    std::lock_guard lifecycle(lifecycle_mutex);
    std::unique_ptr<MqttSession> session;
    {
        std::lock_guard publishing(publish_mutex);
        session.reset(std::exchange(active_session, nullptr));
    }
    if (!session) return;
    session->stopping.store(true);
    auto* client = session->client;
    if (session->connected.load()) {
        const auto topic = session->prefix + "/status";
        mosquitto_publish(client, nullptr, topic.c_str(), static_cast<int>(std::strlen(offline)), offline, 0, true);
    }
    const int rc = mosquitto_disconnect(client);
    mosquitto_loop_stop(client, rc != MOSQ_ERR_SUCCESS);
    mosquitto_destroy(client);
}
