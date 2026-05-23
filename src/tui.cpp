#include "tui.h"
#include "config.h"
#include "util.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <cstring>
#include <atomic>
#include <mutex>

void config_wizard(const std::string& config_path) {
    auto save_cfg = [&]() -> bool {
        std::ofstream f(config_path);
        if(!f.is_open()) return false;
        f << "# ==========================================\n";
        f << "# piTrove Configuration File (v" << VERSION << ")\n";
        f << "# ==========================================\n\n";
        
        f << "[paths]\n";
        f << "media_dir = \"" << g_cfg.media_dir << "\"\n";
        f << "cache_dir = \"" << g_cfg.cache_dir << "\"\n";
        f << "log_dir = \"" << g_cfg.log_dir << "\"\n";
        f << "splash_file = \"" << g_cfg.splash_file << "\"\n\n";
        
        f << "[display]\n";
        f << "resolution = " << g_cfg.screen_w << "," << g_cfg.screen_h << "\n";
        f << "fullscreen = " << (g_cfg.fullscreen ? "1" : "0") << "\n";
        f << "rotation = " << g_cfg.rotation << "\n";
        f << "slideshow_fps = " << g_cfg.slideshow_fps << "\n";
        f << "splash_overlay_y = " << g_cfg.splash_overlay_y << "\n";
        f << "auto_display_rotation = " << (g_cfg.auto_display_rotation ? "1" : "0") << "\n";
        f << "border_enabled = " << (g_cfg.border_enabled ? "1" : "0") << "\n";
        f << "border_width = " << g_cfg.border_width << "\n";
        f << "vignette_enabled = " << (g_cfg.vignette_enabled ? "1" : "0") << "\n\n";
        
        f << "[slideshow]\n";
        f << "transition_delay = " << g_cfg.transition_delay << "\n";
        f << "transition_duration = " << g_cfg.transition_duration << "\n";
        f << "transition_effect = \"" << g_cfg.transition_effect << "\"\n";
        f << "ken_burns = " << (g_cfg.ken_burns ? "1" : "0") << "\n";
        f << "ken_burns_speed = " << g_cfg.ken_burns_speed << "\n";
        f << "ken_burns_zoom = " << g_cfg.ken_burns_zoom << "\n";
        f << "shuffle = " << (g_cfg.shuffle ? "1" : "0") << "\n";
        f << "bias_lighting = " << (g_cfg.bias_lighting ? "1" : "0") << "\n";
        f << "bias_anim_speed = " << g_cfg.bias_anim_speed << "\n";
        f << "bias_anim_style = \"" << g_cfg.bias_anim_style << "\"\n";
        f << "bias_color_mode = \"" << g_cfg.bias_color_mode << "\"\n";
        f << "bias_strength = " << g_cfg.bias_strength << "\n";
        f << "matting = " << (g_cfg.matting ? "1" : "0") << "\n";
        f << "matting_size = " << g_cfg.matting_size << "\n";
        f << "cooldown_days = " << g_cfg.cooldown_days << "\n";
        f << "clock_enabled = " << (g_cfg.clock_enabled ? "1" : "0") << "\n";
        f << "clock_x = " << g_cfg.clock_x << "\n";
        f << "clock_y = " << g_cfg.clock_y << "\n";
        f << "clock_font_size = " << g_cfg.clock_font_size << "\n";
        f << "clock_color = \"" << g_cfg.clock_color << "\"\n";
        f << "clock_24h = " << (g_cfg.clock_24h ? "1" : "0") << "\n\n";
        
        f << "[scan]\n";
        f << "recursive = " << (g_cfg.recursive ? "1" : "0") << "\n";
        f << "depth = " << g_cfg.scan_depth << "\n";
        f << "max_concurrent = " << g_cfg.max_concurrent << "\n";
        f << "window_days = " << g_cfg.scan_window_days << "\n";
        f << "ignore_folders = [";
        for (size_t j = 0; j < g_cfg.ignore_folders.size(); j++)
            f << "\"" << g_cfg.ignore_folders[j] << "\"" << (j < g_cfg.ignore_folders.size() - 1 ? ", " : "");
        f << "]\n\n";
        
        f << "[sqlite]\n";
        f << "mmap_size = " << g_cfg.cache_mmap_size << "\n\n";
        
        f << "[overlay]\n";
        f << "timer_enabled = " << (g_cfg.timer_enabled ? "1" : "0") << "\n";
        f << "timer_x = " << g_cfg.timer_x << "\n";
        f << "timer_y = " << g_cfg.timer_y << "\n";
        f << "timer_font_size = " << g_cfg.timer_font_size << "\n";
        f << "timer_color = \"" << g_cfg.timer_color << "\"\n";
        f << "filename_enabled = " << (g_cfg.filename_enabled ? "1" : "0") << "\n";
        f << "filename_x = " << g_cfg.filename_x << "\n";
        f << "filename_y = " << g_cfg.filename_y << "\n";
        f << "count_enabled = " << (g_cfg.count_enabled ? "1" : "0") << "\n";
        f << "count_x = " << g_cfg.count_x << "\n";
        f << "count_y = " << g_cfg.count_y << "\n";
        f << "videos_per_photos = " << g_cfg.videos_per_photos << "\n";
        f << "play_just_photos = " << (g_cfg.play_just_photos ? "1" : "0") << "\n";
        f << "play_just_videos = " << (g_cfg.play_just_videos ? "1" : "0") << "\n";
        f << "show_people_faces = " << (g_cfg.show_people_faces ? "1" : "0") << "\n";
        f << "keep_animals = " << (g_cfg.keep_animals ? "1" : "0") << "\n";
        f << "sleep_time = " << (g_cfg.sleep_time.empty() ? "\"\"" : "\"" + g_cfg.sleep_time + "\"") << "\n";
        f << "wake_time = " << (g_cfg.wake_time.empty() ? "\"\"" : "\"" + g_cfg.wake_time + "\"") << "\n";
        f << "filename_font_size = " << g_cfg.filename_font_size << "\n";
        f << "count_font_size = " << g_cfg.count_font_size << "\n";
        f << "font_path = \"" << (g_cfg.font_path.empty() ? "auto" : g_cfg.font_path) << "\"\n\n";
        
        f << "[video]\n";
        f << "volume = " << g_cfg.video_volume << "\n";
        f << "probe_timeout = " << g_cfg.video_probe_timeout << "\n";
        f << "closed_captions_enabled = " << (g_cfg.closed_captions_enabled ? "1" : "0") << "\n";
        f << "drm_connector = \"" << (g_cfg.drm_connector.empty() ? "auto" : g_cfg.drm_connector) << "\"\n";
        f << "drm_card = \"" << (g_cfg.drm_card.empty() ? "auto" : g_cfg.drm_card) << "\"\n";
        f << "video_audio_device = \"" << (g_cfg.video_audio_device.empty() ? "auto" : g_cfg.video_audio_device) << "\"\n";
        f << "subtitles_dir = \"" << g_cfg.video_subtitles_dir << "\"\n\n";
        
        f << "[dashboard]\n";
        f << "weather_enabled = " << (g_cfg.weather_enabled ? "1" : "0") << "\n";
        f << "weather_lat = " << g_cfg.weather_lat << "\n";
        f << "weather_lon = " << g_cfg.weather_lon << "\n\n";
        
        f << "[remote]\n";
        f << "http_enabled = " << (g_cfg.http_enabled ? "1" : "0") << "\n";
        f << "http_port = " << g_cfg.http_port << "\n";
        f << "web_dashboard_enabled = " << (g_cfg.web_dashboard_enabled ? "1" : "0") << "\n\n";

        f << "[features]\n";
        f << "on_this_day_enabled = " << (g_cfg.on_this_day_enabled ? "1" : "0") << "\n";
        f << "diagnostics_hud_enabled = " << (g_cfg.diagnostics_hud_enabled ? "1" : "0") << "\n";
        f << "adaptive_text_enabled = " << (g_cfg.adaptive_text_enabled ? "1" : "0") << "\n";
        f << "twin_portrait_enabled = " << (g_cfg.twin_portrait_enabled ? "1" : "0") << "\n\n";
        
        f << "[date_overlay]\n";
        f << "enabled = " << (g_cfg.date_overlay_enabled ? "1" : "0") << "\n";
        f << "text = \"" << g_cfg.date_text << "\"\n";
        f << "x = " << g_cfg.date_x << "\n";
        f << "y = " << g_cfg.date_y << "\n";
        f << "font_size = " << g_cfg.date_font_size << "\n";
        f << "color = \"" << g_cfg.date_color << "\"\n\n";
        
        f << "[brightness]\n";
        f << "auto = " << (g_cfg.brightness_auto ? "1" : "0") << "\n";
        f << "auto_min = " << g_cfg.brightness_auto_min << "\n";
        f << "auto_max = " << g_cfg.brightness_auto_max << "\n\n";
        
        f << "[touch]\n";
        f << "enabled = " << (g_cfg.touch_enabled ? "1" : "0") << "\n\n";
        
        f << "[collage]\n";
        f << "enabled = " << (g_cfg.collage_enabled ? "1" : "0") << "\n";
        f << "cols = " << g_cfg.collage_cols << "\n";
        f << "rows = " << g_cfg.collage_rows << "\n\n";
        
        f << "[log]\n";
        f << "level = \"" << (g_cfg.verbose ? "debug" : "info") << "\"\n\n";
        
        f << "[mqtt]\n";
        f << "enabled = " << (g_cfg.mqtt_enabled ? "1" : "0") << "\n";
        f << "broker = \"" << g_cfg.mqtt_broker << "\"\n";
        f << "port = " << g_cfg.mqtt_port << "\n";
        f << "user = \"" << g_cfg.mqtt_user << "\"\n";
        f << "pass = \"" << g_cfg.mqtt_pass << "\"\n";
        f << "topic_prefix = \"" << g_cfg.mqtt_topic_prefix << "\"\n";
        f << "motionsensor_topic = \"" << g_cfg.mqtt_motionsensor_topic << "\"\n";
        f << "motionsensor_cooldown = " << g_cfg.mqtt_motionsensor_cooldown << "\n";

        f.close();
        g_config_changed.store(true);
        return true;
    };

    // ── TERMINAL SIZING ──
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int term_cols = w.ws_col;
    if (term_cols < 100) {
        printf("\033[8;40;155t"); // Request terminal resize to 155x40
        fflush(stdout);
        usleep(100000);
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        term_cols = w.ws_col;
    }
    int tui_width = std::max(100, std::min(155, term_cols));

    system("stty -icanon -echo");
    printf("\033[?1049h\033[H\033[J");

    enum IT { STR, INT, FLT, TGL, ENM, LST };
    struct CI { const char* n; IT t; const char* desc; };

    // ── DEFINITIONS WITH DESCRIPTIONS ──
    static const CI CA[] = {
        {"Rotation", INT, "Screen rotation in degrees (0, 90, 180, 270)"},
        {"Ken Burns Zoom", FLT, "Zoom intensity for Ken Burns effect (0.0 to 1.0)"},
        {"Auto Display Rotation", TGL, "Rotate images based on EXIF orientation"},
        {"Brightness Auto", TGL, "Auto backlight dimming based on time of day"}
    };
    static const CI CB[] = {
        {"Media Directory", STR, "Root folder containing photos and videos"},
        {"Cache Directory", STR, "Folder for SQLite metadata cache"},
        {"Log Directory", STR, "Folder to store runtime logs"},
        {"Sleep Time", STR, "Time to turn off HDMI port (HH:MM, e.g. 23:00)"},
        {"Wake Time", STR, "Time to turn on HDMI port (HH:MM, e.g. 07:30)"},
        {"HTTP Remote", TGL, "Enable local web server to skip/pause"},
        {"Web Dashboard", TGL, "Enable glassmorphic HTTP web remote control dashboard"},
        {"Splash Overlay Y", FLT, "Vertical position of splash UI (0.0 to 1.0)"}
    };
    static const CI CC[] = {
        {"Timer Enabled", TGL, "Show remaining photo/video duration overlay"},
        {"Timer X Pos", FLT, "Horizontal position of timer (0.0 to 1.0)"},
        {"Timer Y Pos", FLT, "Vertical position of timer (0.0 to 1.0)"},
        {"Timer Size", INT, "Font size of timer text in pixels"},
        {"Timer Color", ENM, "Color of timer text"},
        {"Clock Enabled", TGL, "Show time overlay on screen"},
        {"Clock X Pos", FLT, "Horizontal position of clock (0.0 to 1.0)"},
        {"Clock Y Pos", FLT, "Vertical position of clock (0.0 to 1.0)"},
        {"Clock Size", INT, "Font size of clock text in pixels"},
        {"Clock Color", ENM, "Color of clock text"},
        {"Clock 24h", TGL, "Use 24-hour format for clock"},
        {"Count Enabled", TGL, "Show playlist progress (e.g., '14 / 2054')"},
        {"Diagnostics HUD", TGL, "Monospace OSD overlay showing FPS, SoC temp, SQLite size, tags"},
        {"Adaptive Text", TGL, "Color text outline dynamically based on background brightness"}
    };
    static const CI CD[] = {
        {"Video Volume", INT, "Volume level for video playback (0=muted)"},
        {"Videos per Photos", INT, "Interleave ratio. E.g., '2' plays 2 vids per 10 pics"},
        {"Probe Timeout", INT, "Max seconds for ffprobe duration extraction (0=disabled)"},
        {"Play Just Photos", TGL, "Completely exclude videos from playback"},
        {"Play Just Videos", TGL, "Completely exclude photos from playback"},
        {"Closed Captions", TGL, "Enable video closed captions/subtitles by default"},
        {"Subtitles Dir", STR, "Path to folder containing .srt files (matching video basename)"}
    };
    static const CI CE[] = {
        {"Transition Delay", FLT, "Seconds to display photo before transitioning"},
        {"Transition Duration", FLT, "Seconds the transition animation takes"},
        {"Transition Effect", ENM, "Visual style when swapping (crossfade/wipe/pixelate)"},
        {"Ken Burns", TGL, "Smoothly pan and zoom across static photos"},
        {"Ken Burns Speed", FLT, "Speed multiplier for pan/zoom movement"},
        {"Bias Lighting", TGL, "Enable ambient background glow derived from photo"},
        {"Bias Anim Speed", FLT, "Speed of background glow animation (0.0 to stop)"},
        {"Bias Anim Style", ENM, "Visual style of glow (pulsing/edge_glow/aura)"},
        {"Bias Color Mode", ENM, "Color source for glow (auto/rainbow)"},
        {"Matting Enabled", TGL, "Draw 3D matte border around photos"},
        {"Matting Size", INT, "Thickness of the matte border in pixels"},
        {"Cooldown Days", INT, "Days to wait before showing a photo again (0=off)"},
        {"Shuffle", TGL, "Randomize photo/video order"},
        {"Twin Portrait Split", TGL, "Render consecutive portrait images side-by-side"}
    };
    static const CI CG[] = {
        {"Recursive Scan", TGL, "Recursively scan subdirectories"},
        {"Scan Depth", INT, "Max subdirectory depth to scan"},
        {"Temporal Window", INT, "Show media from =/- X days of today, any year. 0=all"},
        {"Ignore Folders", LST, "Comma-separated folder names to skip"},
        {"Max Concurrent", INT, "Max threads during loading (match CPU cores)"},
        {"Keep People", TGL, "Only show photos containing people (family, faces, friends)"},
        {"Keep Animals", TGL, "Only show photos containing animals (pets, wildlife)"},
        {"On This Day", TGL, "Filter playlist to pictures matching current month and day"}
    };
    static const CI CH[] = {
        {"Weather Enabled", TGL, "Fetch local weather via Open-Meteo API"},
        {"Latitude", FLT, "Location latitude for weather API"},
        {"Longitude", FLT, "Location longitude for weather API"}
    };
    static const CI CF[] = {
        {"DRM Card", STR, "Parent GPU modesetting card path (e.g. '/dev/dri/card1' or 'auto')"},
        {"DRM Connector", STR, "Active connected display connector port (e.g. 'HDMI-A-1' or 'auto')"},
        {"Font Path", STR, "Custom path to TTF/OTF font file (or 'auto' for default search)"},
        {"Audio Device", STR, "Custom audio device identifier for mpv video player (or 'auto')"}
    };
    static const CI CI2[] = {
        {"Log Level", ENM, "Console verbosity (debug, info, warn, error)"},
        {"Min Brightness", INT, "Floor for auto-brightness (0-100)"},
        {"SQLite mmap Size", INT, "Bytes to allocate for DB memory mapping"}
    };
    static const CI CMQ[] = {
        {"MQTT Enabled", TGL, "Enable MQTT features (0/1)"},
        {"MQTT Broker", STR, "IP or domain of the MQTT broker"},
        {"MQTT Port", INT, "Port of the MQTT broker (default 1883)"},
        {"MQTT Username", STR, "Broker username (leave blank if none)"},
        {"MQTT Password", STR, "Broker password (leave blank if none)"},
        {"MQTT Topic Prefix", STR, "Base topic for state/command messages"},
        {"Motion Topic", STR, "MQTT topic that transmits motion events"},
        {"Motion Cooldown", INT, "Screen off delay in seconds when no motion is detected"}
    };

    struct CAT { const char* n; const CI* i; int c; };
    static const CAT CATS[] = {
        {"Display", CA, 4},
        {"System", CB, 8},
        {"Overlays", CC, 14},
        {"Videos", CD, 7},
        {"Slideshow", CE, 14},
        {"Scanning", CG, 8},
        {"Weather", CH, 3},
        {"Hardware", CF, 4},
        {"Advanced", CI2, 3},
        {"MQTT", CMQ, 8}
    };

    // ── DATA ACCESSORS ──
    auto gv = [&](int c, int i) -> std::string {
        if (c == 0) switch(i) {
            case 0: return std::to_string(g_cfg.rotation);
            case 1: return std::to_string(g_cfg.ken_burns_zoom);
            case 2: return g_cfg.auto_display_rotation?"[ON]":"[OFF]";
            case 3: return g_cfg.brightness_auto?"[ON]":"[OFF]";
        }
        if (c == 1) switch(i) {
            case 0: return g_cfg.media_dir; case 1: return g_cfg.cache_dir; case 2: return g_cfg.log_dir;
            case 3: return g_cfg.sleep_time; case 4: return g_cfg.wake_time;
            case 5: return g_cfg.http_enabled?"[ON]":"[OFF]";
            case 6: return g_cfg.web_dashboard_enabled?"[ON]":"[OFF]";
            case 7: return std::to_string(g_cfg.splash_overlay_y);
        }
        if (c == 2) switch(i) {
            case 0: return g_cfg.timer_enabled?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.timer_x);
            case 2: return std::to_string(g_cfg.timer_y);
            case 3: return std::to_string(g_cfg.timer_font_size);
            case 4: return g_cfg.timer_color;
            case 5: return g_cfg.clock_enabled?"[ON]":"[OFF]";
            case 6: return std::to_string(g_cfg.clock_x);
            case 7: return std::to_string(g_cfg.clock_y);
            case 8: return std::to_string(g_cfg.clock_font_size);
            case 9: return g_cfg.clock_color;
            case 10: return g_cfg.clock_24h?"[ON]":"[OFF]";
            case 11: return g_cfg.count_enabled?"[ON]":"[OFF]";
            case 12: return g_cfg.diagnostics_hud_enabled?"[ON]":"[OFF]";
            case 13: return g_cfg.adaptive_text_enabled?"[ON]":"[OFF]";
        }
        if (c == 3) switch(i) {
            case 0: return std::to_string(g_cfg.video_volume);
            case 1: return std::to_string(g_cfg.videos_per_photos);
            case 2: return std::to_string(g_cfg.video_probe_timeout);
            case 3: return g_cfg.play_just_photos?"[ON]":"[OFF]";
            case 4: return g_cfg.play_just_videos?"[ON]":"[OFF]";
            case 5: return g_cfg.closed_captions_enabled?"[ON]":"[OFF]";
            case 6: return g_cfg.video_subtitles_dir;
        }
        if (c == 4) switch(i) {
            case 0: return std::to_string(g_cfg.transition_delay);
            case 1: return std::to_string(g_cfg.transition_duration);
            case 2: return g_cfg.transition_effect;
            case 3: return g_cfg.ken_burns?"[ON]":"[OFF]";
            case 4: return std::to_string(g_cfg.ken_burns_speed);
            case 5: return g_cfg.bias_lighting?"[ON]":"[OFF]";
            case 6: return std::to_string(g_cfg.bias_anim_speed);
            case 7: return g_cfg.bias_anim_style; case 8: return g_cfg.bias_color_mode;
            case 9: return g_cfg.matting?"[ON]":"[OFF]";
            case 10: return std::to_string(g_cfg.matting_size);
            case 11: return std::to_string(g_cfg.cooldown_days);
            case 12: return g_cfg.shuffle?"[ON]":"[OFF]";
            case 13: return g_cfg.twin_portrait_enabled?"[ON]":"[OFF]";
        }
        if (c == 5) switch(i) {
            case 0: return g_cfg.recursive?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.scan_depth);
            case 2: return std::to_string(g_cfg.scan_window_days);
            case 3: { std::string s; for(size_t x=0;x<g_cfg.ignore_folders.size();x++) s+=g_cfg.ignore_folders[x]+(x<g_cfg.ignore_folders.size()-1?",":""); return s; }
            case 4: return std::to_string(g_cfg.max_concurrent);
            case 5: return g_cfg.show_people_faces?"[ON]":"[OFF]";
            case 6: return g_cfg.keep_animals?"[ON]":"[OFF]";
            case 7: return g_cfg.on_this_day_enabled?"[ON]":"[OFF]";
        }
        if (c == 6) switch(i) {
            case 0: return g_cfg.weather_enabled?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.weather_lat);
            case 2: return std::to_string(g_cfg.weather_lon);
        }
        if (c == 7) switch(i) {
            case 0: return g_cfg.drm_card;
            case 1: return g_cfg.drm_connector;
            case 2: return g_cfg.font_path;
            case 3: return g_cfg.video_audio_device;
        }
        if (c == 8) switch(i) {
            case 0: return g_cfg.verbose?"debug":"info";
            case 1: return std::to_string(g_cfg.brightness_auto_min);
            case 2: return std::to_string(g_cfg.cache_mmap_size);
        }
        if (c == 9) switch(i) {
            case 0: return g_cfg.mqtt_enabled ? "[ON]" : "[OFF]";
            case 1: return g_cfg.mqtt_broker;
            case 2: return std::to_string(g_cfg.mqtt_port);
            case 3: return g_cfg.mqtt_user;
            case 4: return g_cfg.mqtt_pass;
            case 5: return g_cfg.mqtt_topic_prefix;
            case 6: return g_cfg.mqtt_motionsensor_topic;
            case 7: return std::to_string(g_cfg.mqtt_motionsensor_cooldown);
        }
        return "";
    };

    auto sv = [&](int c, int i, const std::string& v) {
        if(v.empty()) return;
        std::lock_guard<std::mutex> lk(g_config_mtx);
        try {
            if(c==0) switch(i){
                case 0:{ try { g_cfg.rotation=std::stoi(v); } catch(...) {} break; }
                case 1:{ try { g_cfg.ken_burns_zoom=std::stof(v); } catch(...) {} break; }
                case 2:g_cfg.auto_display_rotation=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 3:g_cfg.brightness_auto=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
            }
            else if(c==1) switch(i){
                case 0:g_cfg.media_dir=v;break; case 1:g_cfg.cache_dir=v;break; case 2:g_cfg.log_dir=v;break;
                case 3:g_cfg.sleep_time=v;break; case 4:g_cfg.wake_time=v;break;
                case 5:g_cfg.http_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:g_cfg.web_dashboard_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 7:{ try { g_cfg.splash_overlay_y=std::stof(v); } catch(...) {} break; }
            }
            else if(c==2) switch(i){
                case 0:g_cfg.timer_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:{ try { g_cfg.timer_x=std::stof(v); } catch(...) {} break; } case 2:{ try { g_cfg.timer_y=std::stof(v); } catch(...) {} break; }
                case 3:{ try { g_cfg.timer_font_size=std::stoi(v); } catch(...) {} break; } case 4:g_cfg.timer_color=v;break;
                case 5:g_cfg.clock_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:{ try { g_cfg.clock_x=std::stof(v); } catch(...) {} break; } case 7:{ try { g_cfg.clock_y=std::stof(v); } catch(...) {} break; }
                case 8:{ try { g_cfg.clock_font_size=std::stoi(v); } catch(...) {} break; } case 9:g_cfg.clock_color=v;break;
                case 10:g_cfg.clock_24h=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 11:g_cfg.count_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 12:g_cfg.diagnostics_hud_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 13:g_cfg.adaptive_text_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
            }
            else if(c==3) switch(i){
                case 0:{ try { g_cfg.video_volume=std::stoi(v); } catch(...) {} break; }
                case 1:{ try { int val = std::stoi(v); g_cfg.videos_per_photos=std::max(1, std::min(100, val)); } catch(...) {} break; }
                case 2:{ try { g_cfg.video_probe_timeout=std::stoi(v); } catch(...) {} break; }
                case 3:g_cfg.play_just_photos=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 4:g_cfg.play_just_videos=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 5:g_cfg.closed_captions_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:g_cfg.video_subtitles_dir=v;break;
            }
            else if(c==4) switch(i){
                case 0:{ try { g_cfg.transition_delay=std::stof(v); } catch(...) {} break; } case 1:{ try { g_cfg.transition_duration=std::stof(v); } catch(...) {} break; }
                case 2:g_cfg.transition_effect=v;break;
                case 3:g_cfg.ken_burns=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 4:{ try { g_cfg.ken_burns_speed=std::stof(v); } catch(...) {} break; }
                case 5:g_cfg.bias_lighting=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:{ try { g_cfg.bias_anim_speed=std::stof(v); } catch(...) {} break; }
                case 7:g_cfg.bias_anim_style=v;break; case 8:g_cfg.bias_color_mode=v;break;
                case 9:g_cfg.matting=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 10:{ try { g_cfg.matting_size=std::stoi(v); } catch(...) {} break; }
                case 11:{ try { g_cfg.cooldown_days=std::stoi(v); } catch(...) {} break; }
                case 12:g_cfg.shuffle=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 13:g_cfg.twin_portrait_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
            }
            else if(c==5) switch(i){
                case 0:g_cfg.recursive=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:{ try { g_cfg.scan_depth=std::stoi(v); } catch(...) {} break; }
                case 2:{ try { g_cfg.scan_window_days=std::stoi(v); } catch(...) {} break; }
                case 3:{
                    g_cfg.ignore_folders.clear();
                    std::string clean = v;
                    clean.erase(std::remove(clean.begin(), clean.end(), '['), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), ']'), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), '"'), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), '\''), clean.end());
                    
                    size_t pos = 0, f;
                    while((f = clean.find(',', pos)) != std::string::npos) {
                        std::string token = clean.substr(pos, f - pos);
                        token.erase(0, token.find_first_not_of(" \t"));
                        token.erase(token.find_last_not_of(" \t") + 1);
                        if (!token.empty()) g_cfg.ignore_folders.push_back(token);
                        pos = f + 1;
                    }
                    std::string last = clean.substr(pos);
                    last.erase(0, last.find_first_not_of(" \t"));
                    last.erase(last.find_last_not_of(" \t") + 1);
                    if (!last.empty()) g_cfg.ignore_folders.push_back(last);
                }break;
                case 4:{ try { g_cfg.max_concurrent=std::stoi(v); } catch(...) {} break; }
                case 5:g_cfg.show_people_faces=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:g_cfg.keep_animals=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 7:g_cfg.on_this_day_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
            }
            else if(c==6) switch(i){
                case 0:g_cfg.weather_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:{ try { float vl=std::stof(v); if(vl>=-90.0f&&vl<=90.0f) g_cfg.weather_lat=vl; } catch(...) {} break; }
                case 2:{ try { float vn=std::stof(v); if(vn>=-180.0f&&vn<=180.0f) g_cfg.weather_lon=vn; } catch(...) {} break; }
            }
            else if(c==7) switch(i){
                case 0:g_cfg.drm_card=v;break;
                case 1:g_cfg.drm_connector=v;break;
                case 2:g_cfg.font_path=v;break;
                case 3:g_cfg.video_audio_device=v;break;
            }
            else if(c==8) switch(i){
                case 0:{ if(v=="debug") g_cfg.verbose=true; else g_cfg.verbose=false; }break;
                case 1:{ try { g_cfg.brightness_auto_min=std::stoi(v); } catch(...) {} break; }
                case 2:{ try { g_cfg.cache_mmap_size=std::stoll(v); } catch(...) {} break; }
            }
            else if(c==9) switch(i){
                case 0:g_cfg.mqtt_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:g_cfg.mqtt_broker=v;break;
                case 2:{ try { g_cfg.mqtt_port=std::stoi(v); } catch(...) {} break; }
                case 3:g_cfg.mqtt_user=v;break;
                case 4:g_cfg.mqtt_pass=v;break;
                case 5:g_cfg.mqtt_topic_prefix=v;break;
                case 6:g_cfg.mqtt_motionsensor_topic=v;break;
                case 7:{ try { g_cfg.mqtt_motionsensor_cooldown=std::stoi(v); } catch(...) {} break; }
            }
        } catch(...) {}
    };

    auto enums = [&](int c, int i) -> std::vector<std::string> {
        if(c==4&&i==2) return {"crossfade","wipe","pixelate","ken_burns"};
        if(c==4&&i==7) return {"pulsing","radiating","absorbing","edge_glow","aura"};
        if(c==4&&i==8) return {"auto","rainbow"};
        if(c==2&&i==4) return {"yellow","white","cyan","red"};
        if(c==2&&i==9) return {"yellow","white","cyan","red"};
        if(c==2&&i==10) return {"yellow","white","cyan","gray"};
        if(c==8&&i==0) return {"debug","info","warn","error"};
        return {};
    };

    int sel = 0, sel_sub = 0;
    bool edit_mode = false;
    std::string ed_buf;
    bool run = true;

    // ── MAIN TUI LOOP ──
    while(run) {
        printf("\033[H\033[J"); // Clear Screen

        // Layout Geometry
        int name_w = 22;
        int val_w = 26;
        int desc_w = tui_width - name_w - val_w - 6;

        // Header
        printf("\033[1;36m  piTrove Configuration Engine v%s\033[0m\n", VERSION);
        printf("  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("━"); printf("\033[0m\n\n");

        // Top Category Bar
        printf("  ");
        for(int i=0; i<9; i++) {
            if(i==sel) printf("\033[7;33m %s \033[0m  ", CATS[i].n);
            else printf("\033[1;37m%s\033[0m  ", CATS[i].n);
        }
        printf("\n\n");

        // Column Headers
        printf("  \033[1;36m%-*s %-*s %-*s\033[0m\n", name_w, "Setting", val_w, "Value", desc_w, "Description");
        printf("  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("─"); printf("\033[0m\n");

        // Rows
        for(int i=0; i<CATS[sel].c; i++) {
            const auto& item = CATS[sel].i[i];
            std::string val = gv(sel, i);

            if (item.t == TGL) {
                val = (val == "1" || val == "[ON]" || val == "[  ON  ]") ? "[  ON  ]" : "[ OFF  ]";
            }

            // Description truncation if terminal gets small
            std::string desc = item.desc;
            if ((int)desc.length() > desc_w) desc = desc.substr(0, desc_w - 3) + "...";

            if(edit_mode && i==sel_sub) {
                printf("  \033[1;32m%-*s \033[7;37m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, ed_buf.c_str(), desc_w, desc.c_str());
            } else if(i==sel_sub) {
                printf("  \033[1;32m%-*s \033[1;37m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, val.c_str(), desc_w, desc.c_str());
            } else {
                printf("  %-*s \033[37m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, val.c_str(), desc_w, desc.c_str());
            }
        }

        // Fill remaining height to prevent bouncing
        for (int i=CATS[sel].c; i<15; i++) printf("\n");

        printf("\n  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("─"); printf("\033[0m\n");

        // Footer / Keybinds
        if(!edit_mode) {
            printf("  \033[1;37m[\xE2\x86\x91\xE2\x86\x93]\033[0m Select    \033[1;37m[\xE2\x86\x90\xE2\x86\x92]\033[0m Category    \033[1;37m[SPACE/ENTER]\033[0m Toggle/Edit    \033[1;32m[S]\033[0m Save    \033[1;31m[Q]\033[0m Quit\n");
        } else {
            printf("  \033[1;32m[ENTER]\033[0m Confirm   \033[1;31m[ESC]\033[0m Cancel      \033[1;37m[\xE2\x86\x91\xE2\x86\x93]\033[0m Cycle Options\n");
        }

        // ── Restart Notice (dynamic) ──
        if (!edit_mode && g_config_changed.load()) {
            printf("\n  \033[1;33m[NOTICE]\033[0m Previous changes detected. Use \033[1;32m[S]\033[0m to save, then \033[1;36mpiTrove --restart\033[0m to apply.\n");
        }

        // INPUT LOOP
        char c;
        if(read(STDIN_FILENO, &c, 1) == 1) {
            if(!edit_mode) {
                if(c == 'q' || c == 'Q') { run = false; }
                else if(c == 's' || c == 'S') { 
                    if(!save_cfg()) { 
                        printf("\033[0;31m[ERROR]\033[0m Failed to save config file.\n"); 
                        usleep(1500000);
                    } else { 
                        printf("\033[1;32m[OK]\033[0m Configuration saved.\n"); 
                        usleep(1000000);
                    } 
                    run = false; 
                }
                else if(c == '\033') {
                    char seq[2];
                    if(read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if(seq[1] == 'A') { if(sel_sub>0) sel_sub--; else if(sel>0){ sel--; sel_sub=CATS[sel].c-1; } } // UP
                        else if(seq[1] == 'B') { if(sel_sub<CATS[sel].c-1) sel_sub++; else if(sel<9){ sel++; sel_sub=0; } } // DOWN
                        else if(seq[1] == 'D') { if(sel>0) { sel--; sel_sub=0; } } // LEFT Category
                        else if(seq[1] == 'C') { if(sel<9) { sel++; sel_sub=0; } } // RIGHT Category
                    }
                }
                else if(c == '\n' || c == '\r' || c == ' ') {
                    if (CATS[sel].i[sel_sub].t == TGL) { // Instantly toggle
                        std::string v = gv(sel, sel_sub);
                        sv(sel, sel_sub, (v=="1"||v=="[ON]"||v=="[  ON  ]") ? "0" : "1");
                    } else if (CATS[sel].i[sel_sub].t == ENM && c == ' ') { // Space quick-cycles enums
                        auto opts = enums(sel, sel_sub);
                        std::string curr = gv(sel, sel_sub);
                        auto it = std::find(opts.begin(), opts.end(), curr);
                        int idx = (it != opts.end()) ? std::distance(opts.begin(), it) : 0;
                        sv(sel, sel_sub, opts[(idx + 1) % opts.size()]);
                    } else if (c != ' ') { // Enter key goes into Edit Mode for text/numbers
                        edit_mode = true;
                        ed_buf = gv(sel, sel_sub);
                    }
                }
            } else { // In Edit Mode
                if(c == '\n' || c == '\r') {
                    sv(sel, sel_sub, ed_buf);
                    edit_mode = false;
                }
                else if(c == '\033') { // Escape or Arrow keys inside edit mode
                    char seq[2];
                    if(read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if(seq[1] == 'A' || seq[1] == 'B') {
                            const auto& item = CATS[sel].i[sel_sub];
                            if(item.t == ENM) {
                                auto opts = enums(sel, sel_sub);
                                if(!opts.empty()) {
                                    auto it = std::find(opts.begin(), opts.end(), ed_buf);
                                    int idx = (it != opts.end()) ? std::distance(opts.begin(), it) : 0;
                                    if (seq[1] == 'B') idx = (idx + 1) % opts.size(); // DOWN
                                    if (seq[1] == 'A') idx = (idx - 1 + opts.size()) % opts.size(); // UP
                                    ed_buf = opts[idx];
                                }
                            }
                        }
                    } else {
                        edit_mode = false; // ESC cancels
                    }
                }
                else if(c == 127 || c == 8) { // Backspace
                    if(!ed_buf.empty()) ed_buf.pop_back();
                }
                else if(c >= 32 && c <= 126) {
                    // Restrict bounds for float/int edits
                    const auto& item = CATS[sel].i[sel_sub];
                    if (item.t == FLT && !isdigit(c) && c != '.' && c != '-') continue;
                    if (item.t == INT && !isdigit(c) && c != '-') continue;
                    ed_buf += c;
                }
            }
        }
    }

    system("stty icanon echo");
    printf("\033[?1049l"); // Restore original terminal buffer
}
