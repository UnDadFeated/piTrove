#include "mqtt.h"
#include "config.h"
#include "util.h"
#include <thread>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <iostream>

void mqtt_publish(const std::string& topic, const std::string& payload, bool retain) {
    if (!g_cfg.mqtt_enabled) return;
    
    std::string cmd = "mosquitto_pub -h " + g_cfg.mqtt_broker +
                      " -p " + std::to_string(g_cfg.mqtt_port);
    if (!g_cfg.mqtt_user.empty()) {
        cmd += " -u '" + g_cfg.mqtt_user + "'";
    }
    if (!g_cfg.mqtt_pass.empty()) {
        cmd += " -P '" + g_cfg.mqtt_pass + "'";
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
    
    // Spawn detached thread to run command in background safely
    std::thread([cmd]() {
        int res = ::system(cmd.c_str());
        (void)res;
    }).detach();
}

void publish_ha_discovery() {
    std::string prefix = g_cfg.mqtt_topic_prefix;
    
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
    if (!g_cfg.mqtt_motionsensor_topic.empty()) {
        std::string motion_sensor_json = "{"
            "\"name\":\"Motion Sensor\","
            "\"unique_id\":\"piTrove_motion_sensor\","
            "\"state_topic\":\"" + g_cfg.mqtt_motionsensor_topic + "\","
            "\"payload_on\":\"ON\","
            "\"payload_off\":\"OFF\","
            "\"device_class\":\"motion\","
            + device_json +
            "}";
        mqtt_publish("homeassistant/binary_sensor/piTrove_motion/config", motion_sensor_json, true);
    }
}

void start_mqtt_client() {
    if (!g_cfg.mqtt_enabled) return;
    
    std::thread([]() {
        std::string cmd = "mosquitto_sub -h " + g_cfg.mqtt_broker +
                          " -p " + std::to_string(g_cfg.mqtt_port);
        if (!g_cfg.mqtt_user.empty()) {
            cmd += " -u '" + g_cfg.mqtt_user + "'";
        }
        if (!g_cfg.mqtt_pass.empty()) {
            cmd += " -P '" + g_cfg.mqtt_pass + "'";
        }
        cmd += " -t '" + g_cfg.mqtt_motionsensor_topic + "'";
        cmd += " -t '" + g_cfg.mqtt_topic_prefix + "/command/#'";
        cmd += " -F \"%t:%p\" 2>/dev/null";

        g_logger.info("Starting MQTT Subscriber: %s", cmd.c_str());
        
        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) {
            g_logger.error("Failed to start MQTT subscriber popen");
            return;
        }

        // Publish Home Assistant auto-discovery configs
        publish_ha_discovery();
        
        // Publish current state
        mqtt_publish(g_cfg.mqtt_topic_prefix + "/status/screen", g_screen_blanked ? "OFF" : "ON", true);

        // Keep track of active motion timestamp initially
        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));

        char buf[512];
        while (g_running && fgets(buf, sizeof(buf), fp)) {
            std::string line(buf);
            line = trim(line);
            if (line.empty()) continue;

            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;

            std::string topic = line.substr(0, colon);
            std::string payload = line.substr(colon + 1);

            g_logger.info("MQTT Received on [%s]: %s", topic.c_str(), payload.c_str());

            if (topic == g_cfg.mqtt_motionsensor_topic) {
                if (payload == "ON" || payload == "1" || payload == "true" || payload == "motion") {
                    g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                    if (g_screen_blanked) {
                        g_logger.info("MQTT motion detected: waking up display");
                        g_screen_blanked = false;
                        int res = ::system("vcgencmd display_power 1");
                        (void)res;
                        mqtt_publish(g_cfg.mqtt_topic_prefix + "/status/screen", "ON", true);
                    }
                }
            } else if (topic == g_cfg.mqtt_topic_prefix + "/command/next") {
                if (payload == "PRESS" || payload == "1" || payload == "true" || payload == "ON" || payload == "next") {
                    g_logger.info("MQTT remote command: Skip Next");
                    g_remote_command.store(1);
                }
            } else if (topic == g_cfg.mqtt_topic_prefix + "/command/prev") {
                if (payload == "PRESS" || payload == "1" || payload == "true" || payload == "prev") {
                    g_logger.info("MQTT remote command: Prev Slide");
                    g_remote_command.store(2);
                }
            } else if (topic == g_cfg.mqtt_topic_prefix + "/command/pause") {
                if (payload == "PRESS" || payload == "1" || payload == "true" || payload == "pause") {
                    g_logger.info("MQTT remote command: Play/Pause Toggle");
                    g_remote_command.store(3);
                }
            } else if (topic == g_cfg.mqtt_topic_prefix + "/command/screen") {
                if (payload == "OFF" || payload == "0" || payload == "false") {
                    if (!g_screen_blanked) {
                        g_logger.info("MQTT remote command: Screen OFF");
                        g_screen_blanked = true;
                        int res = ::system("vcgencmd display_power 0");
                        (void)res;
                        mqtt_publish(g_cfg.mqtt_topic_prefix + "/status/screen", "OFF", true);
                    }
                } else if (payload == "ON" || payload == "1" || payload == "true") {
                    if (g_screen_blanked) {
                        g_logger.info("MQTT remote command: Screen ON");
                        g_screen_blanked = false;
                        int res = ::system("vcgencmd display_power 1");
                        (void)res;
                        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                        mqtt_publish(g_cfg.mqtt_topic_prefix + "/status/screen", "ON", true);
                    }
                }
            }
        }
        pclose(fp);
        g_logger.warn("MQTT subscriber pipe closed");
    }).detach();
}
