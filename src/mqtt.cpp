#include "mqtt.h"
#include "config.h"
#include "util.h"
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <iostream>

#include <queue>
#include <condition_variable>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

static std::thread g_mqtt_thread;
static std::mutex g_mqtt_mtx;
static pid_t g_mqtt_pid = -1;
static int g_mqtt_pipe_fd = -1;

struct MqttPubRequest {
    std::string cmd;
    std::string payload;
};
static std::queue<MqttPubRequest> g_pub_queue;
static std::mutex g_pub_mtx;
static std::condition_variable g_pub_cv;
static std::thread g_pub_worker_thread;
static std::atomic<bool> g_pub_worker_running{false};

static void pub_worker_loop() {
    g_logger.info("MQTT Publisher worker thread started");
    while (g_pub_worker_running.load()) {
        MqttPubRequest req;
        {
            std::unique_lock<std::mutex> lk(g_pub_mtx);
            g_pub_cv.wait(lk, []() { return !g_pub_queue.empty() || !g_pub_worker_running.load(); });
            if (!g_pub_worker_running.load() && g_pub_queue.empty()) {
                break;
            }
            if (!g_pub_queue.empty()) {
                req = std::move(g_pub_queue.front());
                g_pub_queue.pop();
            }
        }
        if (!req.cmd.empty()) {
            FILE* fp = popen(req.cmd.c_str(), "w");
            if (fp) {
                fwrite(req.payload.data(), 1, req.payload.size(), fp);
                pclose(fp);
            } else {
                g_logger.error("MQTT Publisher: popen failed to start publish helper.");
            }
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
        std::shared_lock<std::shared_mutex> lk(g_config_mtx);
        enabled = g_cfg.mqtt_enabled;
        broker = g_cfg.mqtt_broker;
        port = g_cfg.mqtt_port;
        user = g_cfg.mqtt_user;
        pass = g_cfg.mqtt_pass;
    }
    if (!enabled) return;

    std::string cmd = "mosquitto_pub -h '" + escape_shell_arg(broker) + "'" +
                      " -p " + std::to_string(port);
    if (!user.empty()) {
        cmd += " -u '" + escape_shell_arg(user) + "'";
    }
    if (!pass.empty()) {
        cmd += " -P '" + escape_shell_arg(pass) + "'";
    }
    if (retain) {
        cmd += " -r";
    }
    cmd += " -t '" + escape_shell_arg(topic) + "' -s";

    ensure_pub_worker_running();
    {
        std::lock_guard<std::mutex> lk(g_pub_mtx);
        if (g_pub_queue.size() >= 50) {
            g_logger.warn("MQTT: Outbound queue cap reached (50). Dropping publish request for topic '%s'", topic.c_str());
            return;
        }
        g_pub_queue.push({cmd, payload});
    }
    g_pub_cv.notify_one();
}

void publish_ha_discovery() {
    std::string prefix;
    { std::shared_lock<std::shared_mutex> lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
    
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
        { std::shared_lock<std::shared_mutex> lk(g_config_mtx); sensor_topic = g_cfg.mqtt_motionsensor_topic; }
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
        std::shared_lock<std::shared_mutex> lk(g_config_mtx);
        enabled = g_cfg.mqtt_enabled;
    }
    if (!enabled) return;

    {
        std::lock_guard<std::mutex> lk(g_mqtt_mtx);
        if (g_mqtt_thread.joinable()) return;
    }
    
    g_mqtt_thread = std::thread([]() {
        while (g_running.load()) {
            std::string broker, prefix, sensor_topic;
            int port = 1883;
            {
                std::shared_lock<std::shared_mutex> lk(g_config_mtx);
                broker = g_cfg.mqtt_broker;
                port = g_cfg.mqtt_port;
                prefix = g_cfg.mqtt_topic_prefix;
                sensor_topic = g_cfg.mqtt_motionsensor_topic;
            }
            
            std::vector<std::string> args = {"mosquitto_sub", "-h", broker, "-p", std::to_string(port)};
            std::string user, pass;
            {
                std::shared_lock<std::shared_mutex> lk(g_config_mtx);
                user = g_cfg.mqtt_user;
                pass = g_cfg.mqtt_pass;
            }
            if (!user.empty()) {
                args.push_back("-u");
                args.push_back(user);
            }
            if (!pass.empty()) {
                args.push_back("-P");
                args.push_back(pass);
            }
            if (!sensor_topic.empty()) {
                args.push_back("-t");
                args.push_back(sensor_topic);
            }
            args.push_back("-t");
            args.push_back(prefix + "/command/#");
            args.push_back("-F");
            args.push_back("%t:%p");

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const auto& a : args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);

            g_logger.info("Starting MQTT Subscriber via fork+execvp");
            
            int pipefds[2];
            if (pipe(pipefds) < 0) {
                trigger_error(701); // E701: MQTT_BROKER_UNREACHABLE
                g_logger.error("MQTT: pipe creation failed. Retrying in 10 seconds...");
                for (int i = 0; i < 100 && g_running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            pid_t pid = fork();
            if (pid < 0) {
                close(pipefds[0]);
                close(pipefds[1]);
                trigger_error(701); // E701: MQTT_BROKER_UNREACHABLE
                g_logger.error("MQTT: fork failed. Retrying in 10 seconds...");
                for (int i = 0; i < 100 && g_running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            if (pid == 0) {
                // Child process
                dup2(pipefds[1], STDOUT_FILENO);
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDERR_FILENO);
                    close(devnull);
                }
                close(pipefds[0]);
                close(pipefds[1]);

                int max_fd = sysconf(_SC_OPEN_MAX);
                if (max_fd < 0) max_fd = 1024;
                for (int i = 3; i < max_fd; ++i) close(i);

                execvp(argv[0], argv.data());
                _exit(1);
            }

            // Parent process
            close(pipefds[1]);

            {
                std::lock_guard<std::mutex> lk(g_mqtt_mtx);
                g_mqtt_pid = pid;
                g_mqtt_pipe_fd = pipefds[0];
            }

            FILE* fp = fdopen(pipefds[0], "r");
            if (!fp) {
                close(pipefds[0]);
                g_logger.error("MQTT: fdopen failed. Retrying in 10 seconds...");
                {
                    std::lock_guard<std::mutex> lk(g_mqtt_mtx);
                    g_mqtt_pid = -1;
                    g_mqtt_pipe_fd = -1;
                }
                kill(pid, SIGTERM);
                int status;
                waitpid(pid, &status, 0);
                trigger_error(701);
                for (int i = 0; i < 100 && g_running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            // Clear active MQTT connection error upon successful startup
            if (is_error_active(701)) {
                clear_error(701);
            }

            // Publish Home Assistant auto-discovery configs
            publish_ha_discovery();
            
            // Publish current state
            mqtt_publish(prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);

            // Keep track of active motion timestamp initially
            g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));

            char buf[4096];
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

            fclose(fp);
            {
                std::lock_guard<std::mutex> lk(g_mqtt_mtx);
                g_mqtt_pipe_fd = -1;
                if (g_mqtt_pid == pid) {
                    int status;
                    waitpid(pid, &status, 0);
                    g_mqtt_pid = -1;
                }
            }
            
            if (g_running.load()) {
                g_logger.warn("MQTT subscriber connection dropped. Retrying in 10 seconds...");
                trigger_error(701); // E701: MQTT_BROKER_UNREACHABLE
                for (int i = 0; i < 100 && g_running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        g_logger.info("MQTT subscriber thread exiting.");
    });
}

void stop_mqtt_client() {
    pid_t pid_to_kill = -1;
    {
        std::lock_guard<std::mutex> lk(g_mqtt_mtx);
        if (g_mqtt_pid > 0) {
            pid_to_kill = g_mqtt_pid;
            g_mqtt_pid = -1;
        }
    }
    if (pid_to_kill > 0) {
        kill(pid_to_kill, SIGTERM);
        int status;
        for (int i = 0; i < 30; i++) {   // 3 second grace
            if (waitpid(pid_to_kill, &status, WNOHANG) > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(pid_to_kill, SIGKILL);       // ensure dead
        waitpid(pid_to_kill, &status, 0);
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
