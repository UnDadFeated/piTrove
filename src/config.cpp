#include "config.h"
#include "util.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>


static std::string strip_comments(const std::string& line) {
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            in_quotes = !in_quotes;
        } else if ((line[i] == '#' || line[i] == ';') && !in_quotes) {
            return std::string(line.substr(0, i));
        }
    }
    return line;
}

bool Config::load(const std::string& path) {
    this->loaded_path = path;
    std::ifstream f(path);
    if (!f.is_open()) {
        g_active_error_code.store(802); // E802: CONFIG_SECTION_MISSING
        return false;
    }

    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(strip_comments(line));
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        if (auto eq = line.find('='); eq == std::string::npos) {
            trigger_error(801); // E801: TOML_PARSE_FAILURE
            continue;
        } else {

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
        else if (key == "rotation") {
            // Deprecated/Ignored in Modesetting DRM pipeline
            int r = safe_stoi(val, this->rotation);
            if (r != 0 && r != 90 && r != 180 && r != 270) {
                r = 0;
            }
            this->rotation = r;
        }
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
            this->videos_per_photos = std::clamp(parsed, 1, 100);
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
        else if (key == "osd_offset_x")      this->osd_offset_x = safe_stoi(val, this->osd_offset_x);
        else if (key == "osd_offset_y")      this->osd_offset_y = safe_stoi(val, this->osd_offset_y);
        else if (key == "max_texture_dim")   this->max_texture_dim = std::clamp(safe_stoi(val, this->max_texture_dim), 256, 8192);
        else if (key == "http_socket_timeout") this->http_socket_timeout = std::clamp(safe_stoi(val, this->http_socket_timeout), 1, 60);
        else if (key == "http_bind_attempts")  this->http_bind_attempts = std::clamp(safe_stoi(val, this->http_bind_attempts), 1, 100);
        else if (key == "sleep_time")        this->sleep_time = val;

        else if (key == "wake_time")         this->wake_time = val;
        else if (key == "weather_enabled")   this->weather_enabled = (val == "1" || val == "true");
        else if (key == "weather_lat")       this->weather_lat = safe_stof(val, this->weather_lat);
        else if (key == "weather_lon")       this->weather_lon = safe_stof(val, this->weather_lon);
        else if (key == "http_enabled")      this->http_enabled = (val == "1" || val == "true");
        else if (key == "http_port") {
            int p = safe_stoi(val, this->http_port);
            this->http_port = (p >= 1 && p <= 65535) ? p : 9000;
        }
        else if (key == "api_key")         this->http_api_key = val;
        else if (key == "volume")            this->video_volume = std::clamp(safe_stoi(val, this->video_volume), 0, 150);
        else if (key == "probe_timeout")     this->video_probe_timeout = std::clamp(safe_stoi(val, this->video_probe_timeout), 1, 30);
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
            this->transition_duration = std::clamp(d, 0.1, 10.0);
        }
        else if (key == "slideshow_fps")     this->slideshow_fps = safe_stoi(val, this->slideshow_fps);
        else if (key == "transition_effect") this->transition_effect = val;
        else if (key == "ken_burns_speed")   this->ken_burns_speed = std::clamp(safe_stod(val, this->ken_burns_speed), 0.001, 5.0);
        else if (key == "ken_burns")         this->ken_burns = (val == "1" || val == "true");
        else if (key == "matting")           this->matting = (val == "1" || val == "true");
        else if (key == "matting_size")      this->matting_size = std::clamp(safe_stoi(val, this->matting_size), 0, 500);
        else if (key == "bias_lighting")     this->bias_lighting = (val == "1" || val == "true");
        else if (key == "bias_anim_speed")   this->bias_anim_speed = safe_stof(val, this->bias_anim_speed);
        else if (key == "bias_anim_style")   this->bias_anim_style = val;
        else if (key == "bias_color_mode")   this->bias_color_mode = val;
        else if (key == "cooldown_days")     this->cooldown_days = std::clamp(safe_stoi(val, this->cooldown_days), 0, 3650);
        else if (key == "reset_cooldown_on_restart") this->reset_cooldown_on_restart = (val == "1" || val == "true");
        else if (key == "brightness_auto") this->brightness_auto = (val == "1" || val == "true");
        else if (key == "brightness_auto_min") this->brightness_auto_min = std::clamp(safe_stoi(val, this->brightness_auto_min), 0, 100);
        else if (key == "brightness_auto_max") this->brightness_auto_max = std::clamp(safe_stoi(val, this->brightness_auto_max), 0, 100);
        else if (key == "border_enabled")    this->border_enabled = (val == "1" || val == "true");
        else if (key == "border_width")      this->border_width = std::clamp(safe_stoi(val, this->border_width), 0, 250);
        else if (key == "vignette_enabled")  this->vignette_enabled = (val == "1" || val == "true");
        else if (key == "blurred_background") this->blurred_background = !(val == "0" || val == "false");
        else if (key == "color_matched_matte") this->color_matched_matte = !(val == "0" || val == "false");
        else if (key == "bg_style")          this->bg_style = val;
        else if (key == "pattern_brightness" || key == "pattern_offset") this->pattern_offset = std::clamp(safe_stoi(val, this->pattern_offset), 0, 150);
        else if (key == "pattern_style")     this->pattern_style = val;
        else if (key == "pattern_blend_count") this->pattern_blend_count = std::clamp(safe_stoi(val, this->pattern_blend_count), 1, 3);
        else if (key == "blur_radius") {
            int v = std::clamp(safe_stoi(val, this->blur_radius), 6, 24);
            this->blur_radius = v;
        }
        else if (key == "glow_depth") {
            int v = std::clamp(safe_stoi(val, this->glow_depth), 16, 120);
            this->glow_depth = v;
        }
        else if (key == "edge_glow_shadow")  this->edge_glow_shadow = (val == "1" || val == "true");
        else if (key == "matte_opacity") {
            float v = safe_stof(val, this->matte_opacity);
            this->matte_opacity = std::clamp(v, 0.05f, 0.50f);
        }
        else if (key == "vignette_strength") {
            float v = safe_stof(val, this->vignette_strength);
            this->vignette_strength = std::clamp(v, 0.10f, 0.80f);
        }
        else if (key == "shuffle")           this->shuffle = !(val == "0" || val == "false");
        else if (key == "ken_burns_zoom")    this->ken_burns_zoom = safe_stof(val, this->ken_burns_zoom);
        else if (key == "bias_strength")     this->bias_strength = std::clamp(safe_stoi(val, this->bias_strength), 0, 255);
        else if (key == "clock_enabled")     this->clock_enabled = (val == "1" || val == "true");
        else if (key == "clock_x")           this->clock_x = safe_stof(val, this->clock_x);
        else if (key == "clock_y")           this->clock_y = safe_stof(val, this->clock_y);
        else if (key == "clock_font_size")   this->clock_font_size = safe_stoi(val, this->clock_font_size);
        else if (key == "clock_color")       this->clock_color = val;
        else if (key == "clock_24h")         this->clock_24h = (val == "1" || val == "true");
        else if (key == "filename_font_size") this->filename_font_size = safe_stoi(val, this->filename_font_size);
        else if (key == "count_font_size")    this->count_font_size = safe_stoi(val, this->count_font_size);
        else if (key == "recursive")         this->recursive = (val == "1" || val == "true");
        else if (key == "depth")             this->scan_depth = std::clamp(safe_stoi(val, this->scan_depth), 1, 100);
        else if (key == "max_concurrent")    this->max_concurrent = std::clamp(safe_stoi(val, this->max_concurrent), 1, 64);
        else if (key == "window_days")       this->scan_window_days = std::clamp(safe_stoi(val, this->scan_window_days), 0, 365);
        else if (key == "mmap_size")         this->cache_mmap_size = std::clamp(safe_stoll(val, this->cache_mmap_size), 0LL, 268435456LL);
        else if (key == "level")             this->verbose = (val == "debug");
        else if (key == "log_keep_count")    this->log_keep_count = std::clamp(safe_stoi(val, this->log_keep_count), 1, 100);
        else if (key == "preload_capacity")  this->preload_capacity = std::clamp(safe_stoi(val, this->preload_capacity), 1, 32);
        else if (key == "preload_workers")   this->preload_workers = std::clamp(safe_stoi(val, this->preload_workers), 1, 16);
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
        else if (key == "enabled" && section == "google_photos")             this->google_photos_enabled = (val == "1" || val == "true");
        else if (key == "client_id" && section == "google_photos")            this->google_photos_client_id = val;
        else if (key == "client_secret" && section == "google_photos")        this->google_photos_client_secret = val;
        else if (key == "refresh_token" && section == "google_photos")        this->google_photos_refresh_token = val;
        else if (key == "album_id" && section == "google_photos")             this->google_photos_album_id = val;
        else if (key == "sync_interval_mins" && section == "google_photos")   this->google_photos_sync_interval = safe_stoi(val, this->google_photos_sync_interval);
        else if (key == "cache_dir" && section == "google_photos")            this->google_photos_cache_dir = val;
        else if (key == "auto_update" && section == "updates")                this->auto_update = (val == "1" || val == "true");
        else if (key == "auto_update_branch" && section == "updates")         this->auto_update_branch = val;
        else if (key == "enabled" && section == "keepalive")                  this->keepalive_enabled = (val == "1" || val == "true");
        else if (key == "interval_secs" && section == "keepalive")            this->keepalive_interval = safe_stoi(val, this->keepalive_interval);
        else if (key == "gateway_ip" && section == "keepalive")               this->keepalive_gateway = val;
        else if (key == "wifi_interface" && section == "keepalive")           this->keepalive_interface = val;
        else if (key == "resolution") {
            auto comma = val.find(',');
            if (comma != std::string::npos) {
                this->screen_w = safe_stoi(val.substr(0, comma), this->screen_w);
                this->screen_h = safe_stoi(val.substr(comma + 1), this->screen_h);
            }
        }
        else {
            if (g_logger.is_initialized())
                g_logger.warn("UNRECOGNIZED_KEY: [%s] '%s' in config.toml", section.c_str(), key.c_str());
        }
        } // else (eq != npos)
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

bool Config::save(const std::string& path) {
    std::string tmp_path = path + ".tmp";
    std::ofstream f(tmp_path);
    if (!f.is_open()) return false;

    f << "# ==========================================\n";
    f << "# piTrove Configuration File (v" << VERSION << ")\n";
    f << "# ==========================================\n\n";

    f << "[paths]\n";
    f << "media_dir = \"" << this->media_dir << "\"\n";
    f << "cache_dir = \"" << this->cache_dir << "\"\n";
    f << "log_dir = \"" << this->log_dir << "\"\n";
    f << "splash_file = \"" << this->splash_file << "\"\n\n";

    f << "[display]\n";
    f << "resolution = " << this->screen_w << "," << this->screen_h << "\n";
    f << "fullscreen = " << (this->fullscreen ? "1" : "0") << "\n";
    f << "rotation = " << this->rotation << "\n";
    f << "slideshow_fps = " << this->slideshow_fps << "\n";
    f << "splash_overlay_y = " << this->splash_overlay_y << "\n";
    f << "auto_display_rotation = " << (this->auto_display_rotation ? "1" : "0") << "\n";
    f << "border_enabled = " << (this->border_enabled ? "1" : "0") << "\n";
    f << "border_width = " << this->border_width << "\n";
    f << "vignette_enabled = " << (this->vignette_enabled ? "1" : "0") << "\n";
    f << "bg_style = \"" << this->bg_style << "\"\n";
    f << "pattern_brightness = " << this->pattern_offset << "\n";
    f << "pattern_style = \"" << this->pattern_style << "\"\n";
    f << "pattern_blend_count = " << this->pattern_blend_count << "\n";
    f << "blurred_background = " << (this->blurred_background ? "1" : "0") << "\n";
    f << "color_matched_matte = " << (this->color_matched_matte ? "1" : "0") << "\n";
    f << "matte_opacity = " << this->matte_opacity << "\n";
    f << "vignette_strength = " << this->vignette_strength << "\n";
    f << "blur_radius = " << this->blur_radius << "\n";
    f << "glow_depth = " << this->glow_depth << "\n\n";

    f << "[slideshow]\n";
    f << "transition_delay = " << this->transition_delay << "\n";
    f << "transition_duration = " << this->transition_duration << "\n";
    f << "transition_effect = \"" << this->transition_effect << "\"\n";
    f << "ken_burns = " << (this->ken_burns ? "1" : "0") << "\n";
    f << "ken_burns_speed = " << this->ken_burns_speed << "\n";
    f << "ken_burns_zoom = " << this->ken_burns_zoom << "\n";
    f << "shuffle = " << (this->shuffle ? "1" : "0") << "\n";
    f << "bias_lighting = " << (this->bias_lighting ? "1" : "0") << "\n";
    f << "bias_anim_speed = " << this->bias_anim_speed << "\n";
    f << "bias_anim_style = \"" << this->bias_anim_style << "\"\n";
    f << "bias_color_mode = \"" << this->bias_color_mode << "\"\n";
    f << "bias_strength = " << this->bias_strength << "\n";
    f << "edge_glow_shadow = " << (this->edge_glow_shadow ? "1" : "0") << "\n";
    f << "matting = " << (this->matting ? "1" : "0") << "\n";
    f << "matting_size = " << this->matting_size << "\n";
    f << "cooldown_days = " << this->cooldown_days << "\n";
    f << "reset_cooldown_on_restart = " << (this->reset_cooldown_on_restart ? "1" : "0") << "\n";
    f << "preload_capacity = " << this->preload_capacity << "\n";
    f << "preload_workers = " << this->preload_workers << "\n";
    f << "clock_enabled = " << (this->clock_enabled ? "1" : "0") << "\n";
    f << "clock_x = " << this->clock_x << "\n";
    f << "clock_y = " << this->clock_y << "\n";
    f << "clock_font_size = " << this->clock_font_size << "\n";
    f << "clock_color = \"" << this->clock_color << "\"\n";
    f << "clock_24h = " << (this->clock_24h ? "1" : "0") << "\n\n";

    f << "[scan]\n";
    f << "recursive = " << (this->recursive ? "1" : "0") << "\n";
    f << "depth = " << this->scan_depth << "\n";
    f << "max_concurrent = " << this->max_concurrent << "\n";
    f << "window_days = " << this->scan_window_days << "\n";
    f << "ignore_folders = [";
    for (size_t j = 0; j < this->ignore_folders.size(); j++)
        f << "\"" << this->ignore_folders[j] << "\"" << (j < this->ignore_folders.size() - 1 ? ", " : "");
    f << "]\n\n";

    f << "[sqlite]\n";
    f << "mmap_size = " << this->cache_mmap_size << "\n\n";

    f << "[overlay]\n";
    f << "timer_enabled = " << (this->timer_enabled ? "1" : "0") << "\n";
    f << "timer_x = " << this->timer_x << "\n";
    f << "timer_y = " << this->timer_y << "\n";
    f << "timer_font_size = " << this->timer_font_size << "\n";
    f << "timer_color = \"" << this->timer_color << "\"\n";
    f << "filename_enabled = " << (this->filename_enabled ? "1" : "0") << "\n";
    f << "filename_x = " << this->filename_x << "\n";
    f << "filename_y = " << this->filename_y << "\n";
    f << "count_enabled = " << (this->count_enabled ? "1" : "0") << "\n";
    f << "count_x = " << this->count_x << "\n";
    f << "count_y = " << this->count_y << "\n";
    f << "videos_per_photos = " << this->videos_per_photos << "\n";
    f << "play_just_photos = " << (this->play_just_photos ? "1" : "0") << "\n";
    f << "play_just_videos = " << (this->play_just_videos ? "1" : "0") << "\n";
    f << "show_people_faces = " << (this->show_people_faces ? "1" : "0") << "\n";
    f << "keep_animals = " << (this->keep_animals ? "1" : "0") << "\n";
    f << "sleep_time = " << (this->sleep_time.empty() ? "\"\"" : "\"" + this->sleep_time + "\"") << "\n";
    f << "wake_time = " << (this->wake_time.empty() ? "\"\"" : "\"" + this->wake_time + "\"") << "\n";
    f << "filename_font_size = " << this->filename_font_size << "\n";
    f << "count_font_size = " << this->count_font_size << "\n";
    f << "font_path = \"" << (this->font_path.empty() ? "auto" : this->font_path) << "\"\n\n";

    f << "[video]\n";
    f << "volume = " << this->video_volume << "\n";
    f << "probe_timeout = " << this->video_probe_timeout << "\n";
    f << "closed_captions_enabled = " << (this->closed_captions_enabled ? "1" : "0") << "\n";
    f << "drm_connector = \"" << (this->drm_connector.empty() ? "auto" : this->drm_connector) << "\"\n";
    f << "drm_card = \"" << (this->drm_card.empty() ? "auto" : this->drm_card) << "\"\n";
    f << "video_audio_device = \"" << (this->video_audio_device.empty() ? "auto" : this->video_audio_device) << "\"\n";
    f << "subtitles_dir = \"" << this->video_subtitles_dir << "\"\n";
    f << "osd_offset_x = " << this->osd_offset_x << "\n";
    f << "osd_offset_y = " << this->osd_offset_y << "\n";
    f << "max_texture_dim = " << this->max_texture_dim << "\n";
    f << "http_socket_timeout = " << this->http_socket_timeout << "\n";
    f << "http_bind_attempts = " << this->http_bind_attempts << "\n\n";

    f << "[dashboard]\n";
    f << "weather_enabled = " << (this->weather_enabled ? "1" : "0") << "\n";
    f << "weather_lat = " << this->weather_lat << "\n";
    f << "weather_lon = " << this->weather_lon << "\n\n";

    f << "[remote]\n";
    f << "http_enabled = " << (this->http_enabled ? "1" : "0") << "\n";
    f << "http_port = " << this->http_port << "\n";
    f << "web_dashboard_enabled = " << (this->web_dashboard_enabled ? "1" : "0") << "\n";
    f << "api_key = \"" << this->http_api_key << "\"\n\n";

    f << "[features]\n";
    f << "on_this_day_enabled = " << (this->on_this_day_enabled ? "1" : "0") << "\n";
    f << "diagnostics_hud_enabled = " << (this->diagnostics_hud_enabled ? "1" : "0") << "\n";
    f << "adaptive_text_enabled = " << (this->adaptive_text_enabled ? "1" : "0") << "\n";
    f << "twin_portrait_enabled = " << (this->twin_portrait_enabled ? "1" : "0") << "\n\n";

    f << "[date_overlay]\n";
    f << "enabled = " << (this->date_overlay_enabled ? "1" : "0") << "\n";
    f << "text = \"" << this->date_text << "\"\n";
    f << "x = " << this->date_x << "\n";
    f << "y = " << this->date_y << "\n";
    f << "font_size = " << this->date_font_size << "\n";
    f << "color = \"" << this->date_color << "\"\n\n";

    f << "[brightness]\n";
    f << "auto = " << (this->brightness_auto ? "1" : "0") << "\n";
    f << "auto_min = " << this->brightness_auto_min << "\n";
    f << "auto_max = " << this->brightness_auto_max << "\n\n";

    f << "[touch]\n";
    f << "enabled = " << (this->touch_enabled ? "1" : "0") << "\n\n";

    f << "[collage]\n";
    f << "enabled = " << (this->collage_enabled ? "1" : "0") << "\n";
    f << "cols = " << this->collage_cols << "\n";
    f << "rows = " << this->collage_rows << "\n\n";

    f << "[log]\n";
    f << "level = \"" << (this->verbose ? "debug" : "info") << "\"\n";
    f << "log_keep_count = " << this->log_keep_count << "\n\n";

    f << "[mqtt]\n";
    f << "enabled = " << (this->mqtt_enabled ? "1" : "0") << "\n";
    f << "broker = \"" << this->mqtt_broker << "\"\n";
    f << "port = " << this->mqtt_port << "\n";
    f << "user = \"" << this->mqtt_user << "\"\n";
    f << "pass = \"" << this->mqtt_pass << "\"\n";
    f << "topic_prefix = \"" << this->mqtt_topic_prefix << "\"\n";
    f << "motionsensor_topic = \"" << this->mqtt_motionsensor_topic << "\"\n";
    f << "motionsensor_cooldown = " << this->mqtt_motionsensor_cooldown << "\n\n";

    f << "[google_photos]\n";
    f << "enabled = " << (this->google_photos_enabled ? "1" : "0") << "\n";
    f << "client_id = \"" << this->google_photos_client_id << "\"\n";
    f << "client_secret = \"" << this->google_photos_client_secret << "\"\n";
    f << "refresh_token = \"" << this->google_photos_refresh_token << "\"\n";
    f << "album_id = \"" << this->google_photos_album_id << "\"\n";
    f << "sync_interval_mins = " << this->google_photos_sync_interval << "\n";
    f << "cache_dir = \"" << this->google_photos_cache_dir << "\"\n\n";

    f << "[updates]\n";
    f << "auto_update = " << (this->auto_update ? "1" : "0") << "\n";
    f << "auto_update_branch = \"" << this->auto_update_branch << "\"\n\n";

    f << "[keepalive]\n";
    f << "enabled = " << (this->keepalive_enabled ? "1" : "0") << "\n";
    f << "interval_secs = " << this->keepalive_interval << "\n";
    f << "gateway_ip = \"" << this->keepalive_gateway << "\"\n";
    f << "wifi_interface = \"" << this->keepalive_interface << "\"\n";

    f.close();
    if (f.fail()) {
        std::remove(tmp_path.c_str());
        return false;
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        g_logger.error("Config::save: rename failed: %s", std::strerror(errno));
        std::remove(tmp_path.c_str());
        return false;
    }
    g_config_changed.store(true);
    return true;
}
