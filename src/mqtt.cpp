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
#include <spawn.h>
#include <cerrno>
#include <memory>
#include <signal.h>
#include <fcntl.h>
#include <format>

extern char** environ;

static std::jthread g_mqtt_thread;
static std::mutex g_mqtt_lifecycle_mtx;

struct MqttPubRequest {
    std::vector<std::string> args;
    std::string payload;
};
static std::queue<MqttPubRequest> g_pub_queue;
static std::mutex g_pub_mtx;
static std::condition_variable_any g_pub_cv;
static std::jthread g_pub_worker_thread;
static std::atomic<bool> g_pub_worker_running{false};

// Spawn with file actions so a child of this multithreaded process never runs
// C++ code or inherits the frame's display, database, and listening descriptors.
static pid_t spawn_mosquitto(const std::vector<std::string>& args, int input, int output) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return -1;
    int result = input >= 0
        ? posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO)
        : posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (result == 0) result = output >= 0
        ? posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO)
        : posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (result == 0) result = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (result == 0) result = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    pid_t pid = -1;
    if (result == 0) result = posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    return result == 0 ? pid : -1;
}

static bool wait_for_mosquitto(pid_t pid, std::chrono::seconds timeout, std::stop_token stop) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    do {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (!stop.stop_requested() && g_running.load() && std::chrono::steady_clock::now() < deadline);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return false;
}

// Called with a config snapshot held by the caller.
static std::vector<std::string> mosquitto_args(const char* command, const Config& cfg) {
    std::vector<std::string> args = {command, "-h", cfg.mqtt_broker, "-p", std::to_string(cfg.mqtt_port)};
    if (!cfg.mqtt_user.empty()) args.insert(args.end(), {"-u", cfg.mqtt_user});
    if (!cfg.mqtt_pass.empty()) args.insert(args.end(), {"-P", cfg.mqtt_pass});
    if (cfg.mqtt_tls_enabled) {
        if (cfg.mqtt_ca_cert.empty()) args.push_back("--tls-use-os-certs");
        else args.insert(args.end(), {"--cafile", cfg.mqtt_ca_cert});
        args.insert(args.end(), {"--tls-version", "tlsv1.2"});
    }
    return args;
}

static void pub_worker_loop(std::stop_token stop) {
    g_logger.info("MQTT publisher started");
    while (true) {
        MqttPubRequest req;
        {
            std::unique_lock lock(g_pub_mtx);
            if (!g_pub_cv.wait(lock, stop, [] { return !g_pub_queue.empty(); }) ||
                stop.stop_requested()) break;
            req = std::move(g_pub_queue.front());
            g_pub_queue.pop();
        }

        // A private, unlinked file preserves the full payload even if the CLI
        // exits early. Writing to a pipe could block or raise SIGPIPE.
        const auto close_file = [](FILE* file) { fclose(file); };
        std::unique_ptr<FILE, decltype(close_file)> payload(tmpfile(), close_file);
        if (!payload ||
            fwrite(req.payload.data(), 1, req.payload.size(), payload.get()) != req.payload.size() ||
            fseek(payload.get(), 0, SEEK_SET) != 0) {
            g_logger.warn("MQTT: Could not prepare publish payload");
            continue;
        }
        const auto pid = spawn_mosquitto(req.args, fileno(payload.get()), -1);
        payload.reset();
        if (pid < 0 || !wait_for_mosquitto(pid, std::chrono::seconds(10), stop)) {
            if (!stop.stop_requested() && g_running.load())
                g_logger.warn("MQTT: Publish failed or timed out");
        }
    }
    g_logger.info("MQTT publisher stopped");
}

void mqtt_publish(const std::string& topic, const std::string& payload, bool retain) {
    std::vector<std::string> args;
    {
        std::shared_lock lock(g_config_mtx);
        if (!g_cfg.mqtt_enabled) return;
        args = mosquitto_args("mosquitto_pub", g_cfg);
    }
    if (retain) args.push_back("-r");
    args.insert(args.end(), {"-t", topic, "-s"});

    {
        std::lock_guard lock(g_pub_mtx);
        if (!g_running.load()) return;
        if (!g_pub_worker_running.load()) {
            if (!spawn_thread_safe(g_pub_worker_thread, "mqtt_pub_worker", pub_worker_loop)) return;
            g_pub_worker_running.store(true);
        }
        if (g_pub_queue.size() >= 50) {
            g_logger.warn("MQTT: Outbound queue cap reached (50). Dropping publish request for topic '{}'", topic);
            return;
        }
        g_pub_queue.push({std::move(args), payload});
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
        "\"model\":\"v17.x Modular Frame\","
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
    if (!enabled || !g_running.load()) return;

    std::lock_guard lifecycle(g_mqtt_lifecycle_mtx);
    if (g_mqtt_thread.joinable()) return;
    spawn_thread_safe(g_mqtt_thread, "mqtt", [](std::stop_token stop) {
        const auto active = [&] { return g_running.load() && !stop.stop_requested(); };
        const auto retry = [&] {
            for (int i = 0; i < 100 && active(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        };
        while (active()) {
            std::string prefix, sensor_topic;
            std::vector<std::string> args;
            {
                std::shared_lock lock(g_config_mtx);
                args = mosquitto_args("mosquitto_sub", g_cfg);
                prefix = g_cfg.mqtt_topic_prefix;
                sensor_topic = g_cfg.mqtt_motionsensor_topic;
            }
            if (!sensor_topic.empty()) args.insert(args.end(), {"-t", sensor_topic});
            args.insert(args.end(), {"-t", prefix + "/command/#", "-F", "%t:%p"});

            int pipefds[2];
            if (pipe2(pipefds, O_CLOEXEC) < 0) {
                trigger_error(701);
                g_logger.error("MQTT: Pipe creation failed. Retrying in 10 seconds.");
                retry();
                continue;
            }
            const auto pid = spawn_mosquitto(args, -1, pipefds[1]);
            close(pipefds[1]);
            if (pid < 0) {
                close(pipefds[0]);
                trigger_error(701);
                g_logger.error("MQTT: Could not start the subscriber. Retrying in 10 seconds.");
                retry();
                continue;
            }
            FILE* fp = fdopen(pipefds[0], "r");
            if (!fp) {
                close(pipefds[0]);
                kill(pid, SIGKILL);
                wait_for_mosquitto(pid, std::chrono::seconds(2), stop);
                trigger_error(701);
                retry();
                continue;
            }

            {
                // Wake fgets on stop without racing the worker's waitpid.
                std::stop_callback cancel_subscriber(stop, [pid] { kill(pid, SIGKILL); });
                // Clear active MQTT connection error upon successful startup
                if (is_error_active(701)) {
                    clear_error(701);
                }

                // Publish Home Assistant auto-discovery configs
                publish_ha_discovery();

                // Publish birth (online) message
                mqtt_publish(prefix + "/status", R"({"state":"online"})", true);
            
                // Publish current state
                mqtt_publish(prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);

                // Keep track of active motion timestamp initially
                g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));

                char buf[4096];
                while (active() && fgets(buf, sizeof(buf), fp)) {
                    std::string line(buf);
                    line = trim(line);
                    if (line.empty()) continue;

                    size_t colon = line.find(':');
                    if (colon == std::string::npos) continue;

                    std::string topic = line.substr(0, colon);
                    std::string payload = line.substr(colon + 1);

                    g_logger.info("MQTT Received on [{}]: {}", topic.c_str(), payload.c_str());

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
            }
            // This worker owns reaping from spawn to exit.
            wait_for_mosquitto(pid, std::chrono::seconds(2), stop);
            if (active()) {
                g_logger.warn("MQTT subscriber connection dropped. Retrying in 10 seconds.");
                trigger_error(701);
                retry();
            }
        }
        g_logger.info("MQTT subscriber stopped");
    });
}

void stop_mqtt_client() {
    std::lock_guard lifecycle(g_mqtt_lifecycle_mtx);
    g_mqtt_thread.request_stop();
    if (g_mqtt_thread.joinable()) g_mqtt_thread.join();

    std::jthread publisher;
    {
        std::lock_guard lock(g_pub_mtx);
        g_pub_worker_running.store(false);
        g_pub_queue = {};
        publisher = std::move(g_pub_worker_thread);
        publisher.request_stop();
    }
    g_pub_cv.notify_all();
    if (publisher.joinable()) publisher.join();
}
