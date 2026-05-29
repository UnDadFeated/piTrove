#include "mqtt.h"
#include "config.h"
#include "util.h"
#include <thread>
#include <mutex>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <iostream>

#include <queue>
#include <condition_variable>

static std::thread g_mqtt_thread;
static std::mutex g_mqtt_mtx;
static FILE* g_mqtt_fp = nullptr;

static std::queue<std::string> g_pub_queue;
static std::mutex g_pub_mtx;
static std::condition_variable g_pub_cv;
static std::thread g_pub_worker_thread;
static std::atomic<bool> g_pub_worker_running{false};

static void pub_worker_loop() {
    g_logger.info("MQTT Publisher worker thread started");
    while (g_pub_worker_running.load()) {
        std::string cmd;
        {
            std::unique_lock<std::mutex> lk(g_pub_mtx);
            g_pub_cv.wait(lk, []() { return !g_pub_queue.empty() || !g_pub_worker_running.load(); });
            if (!g_pub_worker_running.load() && g_pub_queue.empty()) {
                break;
            }
            if (!g_pub_queue.empty()) {
                cmd = std::move(g_pub_queue.front());
                g_pub_queue.pop();
            }
        }
        if (!cmd.empty()) {
            int res = ::system(cmd.c_str());
            (void)res;
        }
    }
    g_logger.info("MQTT Publisher worker thread stopped");
}

static void ensure_pub_worker_running() {
    std::lock_guard<std::mutex> lk(g_pub_mtx);
    if (!g_running.load()) return;
    if (!g_pub_worker_running.load()) {
        if (g_pub_worker_thread.joinable()) {
            g_pub_worker_thread.join();
        }
        g_pub_worker_running.store(true);
        g_pub_worker_thread = std::thread(pub_worker_loop);
    }
}

void mqtt_publish(const std::string& topic, const std::string& payload, bool retain) {
    bool enabled;
    std::string broker;
    int port = 1883;
    std::string user, pass;
    {
        std::lock_guard<std::mutex> lk(g_config_mtx);
        enabled = g_cfg.mqtt_enabled;
        broker = g_cfg.mqtt_broker;
        port = g_cfg.mqtt_port;
        user = g_cfg.mqtt_user;
        pass = g_cfg.mqtt_pass;
    }
    if (!enabled) return;

    std::string cmd = "mosquitto_pub -h " + broker +
                      " -p " + std::to_string(port);
    if (!user.empty()) {
        cmd += " -u '" + user + "'";
    }
    if (!pass.empty()) {
        cmd += " -P '" + pass + "'";
    }
    if (retain) {
        cmd += " -r";
    }
    // Escape single quotes in payload
    std::string escaped_payload;
    for (char c : payload) {
        if (c == '\'') escaped_payload += "'\\''";
        else escaped_payload += c;
    }
    cmd += " -t '" + topic + "' -m '" + escaped_payload + "'";

    ensure_pub_worker_running();
    {
        std::lock_guard<std::mutex> lk(g_pub_mtx);
        g_pub_queue.push(cmd);
    }
    g_pub_cv.notify_one();
}

void publish_ha_discovery() {
    std::string prefix;
    { std::lock_guard<std::mutex> lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
    
    // Device configuration block
    std::string device_json = "\"device\":{"
        "\"identifiers\":[\"piTrove_frame\"],"
        "\"name\":\"piTrove Picture Frame\","
        "\"model\":\"v11.x Modular Frame\","
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
        { std::lock_guard<std::mutex> lk(g_config_mtx); sensor_topic = g_cfg.mqtt_motionsensor_topic; }
        if (!sensor_topic.empty()) {
            std::string motion_sensor_json = "{"
                "\"name\":\"Motion Sensor\","
                "\"unique_id\":\"piTrove_motion_sensor\","
                "\"state_topic\":\"" + sensor_topic + "\","
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
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lk(g_config_mtx);
        enabled = g_cfg.mqtt_enabled;
    }
    if (!enabled) return;

    {
        std::lock_guard<std::mutex> lk(g_mqtt_mtx);
        if (g_mqtt_thread.joinable()) return;
    }
    
    g_mqtt_thread = std::thread([]() {
        std::string broker, prefix, sensor_topic;
        int port = 1883;
        {
            std::lock_guard<std::mutex> lk(g_config_mtx);
            broker = g_cfg.mqtt_broker;
            port = g_cfg.mqtt_port;
            prefix = g_cfg.mqtt_topic_prefix;
            sensor_topic = g_cfg.mqtt_motionsensor_topic;
        }
        
        std::string cmd = "mosquitto_sub -h " + broker +
                          " -p " + std::to_string(port);
        std::string user, pass;
        {
            std::lock_guard<std::mutex> lk(g_config_mtx);
            user = g_cfg.mqtt_user;
            pass = g_cfg.mqtt_pass;
        }
        if (!user.empty()) {
            cmd += " -u '" + user + "'";
        }
        if (!pass.empty()) {
            cmd += " -P '" + pass + "'";
        }
        cmd += " -t '" + sensor_topic + "'";
        cmd += " -t '" + prefix + "/command/#'";
        cmd += " -F \"%t:%p\" 2>/dev/null";

        g_logger.info("Starting MQTT Subscriber: %s", cmd.c_str());
        
        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) {
            g_logger.error("Failed to start MQTT subscriber popen");
            return;
        }
        
        {
            std::lock_guard<std::mutex> lk(g_mqtt_mtx);
            g_mqtt_fp = fp;
        }

        // Publish Home Assistant auto-discovery configs
        publish_ha_discovery();
        
        // Publish current state
        mqtt_publish(prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);

        // Keep track of active motion timestamp initially
        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));

        char buf[512];
        while (g_running.load() && fgets(buf, sizeof(buf), fp)) {
            std::string line(buf);
            line = trim(line);
            if (line.empty()) continue;

            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;

            std::string topic = line.substr(0, colon);
            std::string payload = line.substr(colon + 1);

            g_logger.info("MQTT Received on [%s]: %s", topic.c_str(), payload.c_str());

            if (topic == sensor_topic) {
                if (payload == "ON" || payload == "1" || payload == "true" || payload == "motion") {
                    g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                    if (g_screen_blanked.load()) {
                        g_logger.info("MQTT motion detected: waking up display");
                        g_screen_blanked.store(false);
                        set_display_power(true);
                        mqtt_publish(prefix + "/status/screen", "ON", true);
                    }
                }
            } else if (topic == prefix + "/command/next") {
                if (payload == "PRESS" || payload == "1" || payload == "true" || payload == "ON" || payload == "next") {
                    g_logger.info("MQTT remote command: Skip Next");
                    g_remote_command.store(1);
                }
            } else if (topic == prefix + "/command/prev") {
                if (payload == "PRESS" || payload == "1" || payload == "true" || payload == "prev") {
                    g_logger.info("MQTT remote command: Prev Slide");
                    g_remote_command.store(2);
                }
            } else if (topic == prefix + "/command/pause") {
                if (payload == "PRESS" || payload == "1" || payload == "true" || payload == "pause") {
                    g_logger.info("MQTT remote command: Play/Pause Toggle");
                    g_remote_command.store(3);
                }
            } else if (topic == prefix + "/command/screen") {
                if (payload == "OFF" || payload == "0" || payload == "false") {
                    if (!g_screen_blanked.load()) {
                        g_logger.info("MQTT remote command: Screen OFF");
                        g_screen_blanked.store(true);
                        set_display_power(false);
                        mqtt_publish(prefix + "/status/screen", "OFF", true);
                    }
                } else if (payload == "ON" || payload == "1" || payload == "true") {
                    if (g_screen_blanked.load()) {
                        g_logger.info("MQTT remote command: Screen ON");
                        g_screen_blanked.store(false);
                        set_display_power(true);
                        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                        mqtt_publish(prefix + "/status/screen", "ON", true);
                    }
                }
            }
        }
        FILE* fp_to_close = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mqtt_mtx);
            if (g_mqtt_fp) {
                fp_to_close = g_mqtt_fp;
                g_mqtt_fp = nullptr;
            }
        }
        if (fp_to_close) {
            pclose(fp_to_close);
        }
        
        g_logger.warn("MQTT subscriber pipe closed");
     });
}

void stop_mqtt_client() {
    FILE* fp_to_close = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mqtt_mtx);
        if (g_mqtt_fp) {
            fp_to_close = g_mqtt_fp;
            g_mqtt_fp = nullptr;
        }
    }
    if (fp_to_close) {
        pclose(fp_to_close);
    }
    if (g_mqtt_thread.joinable()) {
        g_mqtt_thread.join();
    }
    
    // Stop the publisher worker thread cleanly
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lk(g_pub_mtx);
        if (g_pub_worker_running.load()) {
            g_pub_worker_running.store(false);
            if (g_pub_worker_thread.joinable()) {
                to_join = std::move(g_pub_worker_thread);
            }
        }
    }
    g_pub_cv.notify_all();
    if (to_join.joinable()) {
        to_join.join();
    }
}
