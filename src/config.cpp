#include "config.h"
#include "util.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>

Config g_cfg;
std::mutex g_config_mtx;

bool Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }

        if (key == "media_dir")              this->media_dir = val;
        else if (key == "cache_dir")         this->cache_dir = val;
        else if (key == "log_dir")           this->log_dir = val;
        else if (key == "splash_file")       this->splash_file = val;
        else if (key == "fullscreen")        this->fullscreen = (val == "1" || val == "true");
        else if (key == "rotation")          this->rotation = safe_stoi(val, this->rotation);
        else if (key == "splash_overlay_y")  this->splash_overlay_y = safe_stof(val, this->splash_overlay_y);
        else if (key == "timer_enabled")     this->timer_enabled = (val == "1" || val == "true");
        else if (key == "timer_x")           this->timer_x = safe_stof(val, this->timer_x);
        else if (key == "timer_y")           this->timer_y = safe_stof(val, this->timer_y);
        else if (key == "timer_font_size")   this->timer_font_size = safe_stoi(val, this->timer_font_size);
        else if (key == "timer_color")        this->timer_color = val;
        else if (key == "filename_enabled")  this->filename_enabled = (val == "1" || val == "true");
        else if (key == "filename_x")        this->filename_x = safe_stof(val, this->filename_x);
        else if (key == "filename_y")        this->filename_y = safe_stof(val, this->filename_y);
        else if (key == "count_enabled")     this->count_enabled = (val == "1" || val == "true");
        else if (key == "count_x")           this->count_x = safe_stof(val, this->count_x);
        else if (key == "count_y")           this->count_y = safe_stof(val, this->count_y);
        else if (key == "videos_per_photos") {
            int parsed = safe_stoi(val, 3);
            this->videos_per_photos = std::max(1, std::min(100, parsed));
        }
        else if (key == "play_just_photos")  this->play_just_photos = (val == "1" || val == "true");
        else if (key == "play_just_videos")  this->play_just_videos = (val == "1" || val == "true");
        else if (key == "show_people_faces") this->show_people_faces = !(val == "0" || val == "false");
        else if (key == "keep_animals")      this->keep_animals = !(val == "0" || val == "false");
        else if (key == "on_this_day_enabled") this->on_this_day_enabled = (val == "1" || val == "true");
        else if (key == "web_dashboard_enabled") this->web_dashboard_enabled = !(val == "0" || val == "false");
        else if (key == "diagnostics_hud_enabled") this->diagnostics_hud_enabled = (val == "1" || val == "true");
        else if (key == "adaptive_text_enabled") this->adaptive_text_enabled = !(val == "0" || val == "false");
        else if (key == "twin_portrait_enabled") this->twin_portrait_enabled = !(val == "0" || val == "false");
        else if (key == "closed_captions_enabled") this->closed_captions_enabled = !(val == "0" || val == "false");
        else if (key == "drm_connector")     this->drm_connector = val;
        else if (key == "drm_card")          this->drm_card = val;
        else if (key == "font_path")         this->font_path = val;
        else if (key == "video_audio_device")this->video_audio_device = val;
        else if (key == "subtitles_dir")     this->video_subtitles_dir = val;
        else if (key == "sleep_time")        this->sleep_time = val;

        else if (key == "wake_time")         this->wake_time = val;
        else if (key == "weather_enabled")   this->weather_enabled = (val == "1" || val == "true");
        else if (key == "weather_lat")       this->weather_lat = safe_stof(val, this->weather_lat);
        else if (key == "weather_lon")       this->weather_lon = safe_stof(val, this->weather_lon);
        else if (key == "http_enabled")      this->http_enabled = (val == "1" || val == "true");
        else if (key == "http_port") {
            int p = safe_stoi(val, this->http_port);
            this->http_port = (p >= 1 && p <= 65535) ? p : 8080;
        }
        else if (key == "volume")            this->video_volume = safe_stoi(val, this->video_volume);
        else if (key == "probe_timeout")     this->video_probe_timeout = std::max(1, std::min(30, safe_stoi(val, this->video_probe_timeout)));
        else if (key == "enabled" && section == "date_overlay") this->date_overlay_enabled = (val == "1" || val == "true");
        else if (key == "text" && section == "date_overlay")    this->date_text = val;
        else if (key == "x" && section == "date_overlay")       this->date_x = safe_stof(val, this->date_x);
        else if (key == "y" && section == "date_overlay")       this->date_y = safe_stof(val, this->date_y);
        else if (key == "font_size" && section == "date_overlay") this->date_font_size = safe_stoi(val, this->date_font_size);
        else if (key == "color" && section == "date_overlay")   this->date_color = val;
        else if (key == "enabled" && section == "touch")        this->touch_enabled = (val == "1" || val == "true");
        else if (key == "enabled" && section == "collage")      this->collage_enabled = (val == "1" || val == "true");
        else if (key == "cols")              this->collage_cols = safe_stoi(val, this->collage_cols);
        else if (key == "rows")              this->collage_rows = safe_stoi(val, this->collage_rows);
        else if (key == "transition_delay")  this->transition_delay = std::max(1.0, safe_stod(val, this->transition_delay));
        else if (key == "transition_duration") {
            double d = safe_stod(val, 1.5);
            this->transition_duration = std::max(0.1, std::min(d, 10.0));
        }
        else if (key == "slideshow_fps")     this->slideshow_fps = safe_stoi(val, this->slideshow_fps);
        else if (key == "transition_effect") this->transition_effect = val;
        else if (key == "ken_burns_speed")   this->ken_burns_speed = std::max(0.001, std::min(5.0, safe_stod(val, this->ken_burns_speed)));
        else if (key == "ken_burns")         this->ken_burns = (val == "1" || val == "true");
        else if (key == "matting")           this->matting = (val == "1" || val == "true");
        else if (key == "matting_size")      this->matting_size = std::max(0, std::min(500, safe_stoi(val, this->matting_size)));
        else if (key == "bias_lighting")     this->bias_lighting = (val == "1" || val == "true");
        else if (key == "bias_anim_speed")   this->bias_anim_speed = safe_stof(val, this->bias_anim_speed);
        else if (key == "bias_anim_style")   this->bias_anim_style = val;
        else if (key == "bias_color_mode")   this->bias_color_mode = val;
        else if (key == "cooldown_days")     this->cooldown_days = std::max(0, std::min(3650, safe_stoi(val, this->cooldown_days)));
        else if (key == "reset_cooldown_on_restart") this->reset_cooldown_on_restart = (val == "1" || val == "true");
        else if (key == "brightness_auto") this->brightness_auto = (val == "1" || val == "true");
        else if (key == "brightness_auto_min") this->brightness_auto_min = std::max(0, std::min(100, safe_stoi(val, this->brightness_auto_min)));
        else if (key == "brightness_auto_max") this->brightness_auto_max = std::max(0, std::min(100, safe_stoi(val, this->brightness_auto_max)));
        else if (key == "border_enabled")    this->border_enabled = (val == "1" || val == "true");
        else if (key == "border_width")      this->border_width = safe_stoi(val, this->border_width);
        else if (key == "vignette_enabled")  this->vignette_enabled = (val == "1" || val == "true");
        else if (key == "blurred_background") this->blurred_background = !(val == "0" || val == "false");
        else if (key == "color_matched_matte") this->color_matched_matte = !(val == "0" || val == "false");
        else if (key == "blur_radius") {
            int v = std::max(6, std::min(24, safe_stoi(val, this->blur_radius)));
            this->blur_radius = v;
        }
        else if (key == "glow_depth") {
            int v = std::max(16, std::min(120, safe_stoi(val, this->glow_depth)));
            this->glow_depth = v;
        }
        else if (key == "matte_opacity") {
            float v = safe_stof(val, this->matte_opacity);
            this->matte_opacity = std::max(0.05f, std::min(0.50f, v));
        }
        else if (key == "vignette_strength") {
            float v = safe_stof(val, this->vignette_strength);
            this->vignette_strength = std::max(0.10f, std::min(0.80f, v));
        }
        else if (key == "shuffle")           this->shuffle = !(val == "0" || val == "false");
        else if (key == "ken_burns_zoom")    this->ken_burns_zoom = safe_stof(val, this->ken_burns_zoom);
        else if (key == "bias_strength")     this->bias_strength = safe_stoi(val, this->bias_strength);
        else if (key == "clock_enabled")     this->clock_enabled = (val == "1" || val == "true");
        else if (key == "clock_x")           this->clock_x = safe_stof(val, this->clock_x);
        else if (key == "clock_y")           this->clock_y = safe_stof(val, this->clock_y);
        else if (key == "clock_font_size")   this->clock_font_size = safe_stoi(val, this->clock_font_size);
        else if (key == "clock_color")       this->clock_color = val;
        else if (key == "clock_24h")         this->clock_24h = (val == "1" || val == "true");
        else if (key == "filename_font_size") this->filename_font_size = safe_stoi(val, this->filename_font_size);
        else if (key == "count_font_size")    this->count_font_size = safe_stoi(val, this->count_font_size);
        else if (key == "recursive")         this->recursive = (val == "1" || val == "true");
        else if (key == "depth")             this->scan_depth = std::max(1, std::min(100, safe_stoi(val, this->scan_depth)));
        else if (key == "max_concurrent")    this->max_concurrent = std::max(1, std::min(64, safe_stoi(val, this->max_concurrent)));
        else if (key == "window_days")       this->scan_window_days = std::max(0, std::min(365, safe_stoi(val, this->scan_window_days)));
        else if (key == "mmap_size")         this->cache_mmap_size = std::max(0LL, std::min(268435456LL, safe_stoll(val, this->cache_mmap_size)));
        else if (key == "level")             this->verbose = (val == "debug");
        else if (key == "log_keep_count")    this->log_keep_count = std::max(1, std::min(100, safe_stoi(val, this->log_keep_count)));
        else if (key == "preload_capacity")  this->preload_capacity = std::max(1, std::min(32, safe_stoi(val, this->preload_capacity)));
        else if (key == "preload_workers")   this->preload_workers = std::max(1, std::min(16, safe_stoi(val, this->preload_workers)));
        else if (key == "ignore_folders") {
            // Parse TOML array: ["@eaDir", "@Recycle"]
            this->ignore_folders.clear();
            auto start = val.find('[');
            auto end = val.rfind(']');
            if (start != std::string::npos && end != std::string::npos) {
                std::string inner = val.substr(start + 1, end - start - 1);
                size_t pos = 0;
                while (pos < inner.size()) {
                    size_t q1 = inner.find('"', pos);
                    if (q1 == std::string::npos) break;
                    size_t q2 = inner.find('"', q1 + 1);
                    if (q2 == std::string::npos) break;
                    std::string item = inner.substr(q1 + 1, q2 - q1 - 1);
                    if (!item.empty()) this->ignore_folders.push_back(item);
                    pos = q2 + 1;
                }
            }
        }
        else if (key == "enabled" && section == "mqtt")             this->mqtt_enabled = (val == "1" || val == "true");
        else if (key == "broker" && section == "mqtt")              this->mqtt_broker = val;
        else if (key == "port" && section == "mqtt")                this->mqtt_port = safe_stoi(val, this->mqtt_port);
        else if (key == "user" && section == "mqtt")                this->mqtt_user = val;
        else if (key == "pass" && section == "mqtt")                this->mqtt_pass = val;
        else if (key == "topic_prefix" && section == "mqtt")        this->mqtt_topic_prefix = val;
        else if (key == "motionsensor_topic" && section == "mqtt")  this->mqtt_motionsensor_topic = val;
        else if (key == "motionsensor_cooldown" && section == "mqtt") this->mqtt_motionsensor_cooldown = safe_stoi(val, this->mqtt_motionsensor_cooldown);
        else if (key == "resolution") {
            auto comma = val.find(',');
            if (comma != std::string::npos) {
                this->screen_w = safe_stoi(val.substr(0, comma), this->screen_w);
                this->screen_h = safe_stoi(val.substr(comma + 1), this->screen_h);
            }
        }
        else {
            g_logger.warn("UNRECOGNIZED_KEY: [%s] '%s' in config.toml", section.c_str(), key.c_str());
        }
    }

    return true;
}

void Config::parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dry-run") {
            this->dry_run = true;
        } else if (arg == "--config" && i + 1 < argc) {
            // Handled inside main startup sequence
            i++;
        }
    }
}
