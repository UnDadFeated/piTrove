#ifndef PITROVE_CONFIG_H
#define PITROVE_CONFIG_H

#include <cmath>
#include <string>
#include <vector>
#include <mutex>

struct Config {
    std::string media_dir;
    std::string cache_dir;
    std::string log_dir;
    std::string splash_file;
    int     screen_w{1920};
    int     screen_h{1080};
    bool    fullscreen{false};
    int     rotation{0};
    double  transition_delay{120.0};
    double  transition_duration{1.5};
    std::string transition_effect{"crossfade"};
    double  ken_burns_speed{0.1};
    bool    ken_burns{false};
    bool    matting{true};
    int     matting_size{96};
    bool    bias_lighting{true};
    float   bias_anim_speed{0.5f};
    std::string bias_anim_style{"edge_glow"};
    std::string bias_color_mode{"auto"};
    float   splash_overlay_y{0.5f};
    int     scan_depth{10};
    int     max_concurrent{4};
    bool    recursive{true};
    int     scan_window_days{5};
    long long cache_mmap_size{67108864};
    bool    verbose{false};
    int     slideshow_fps{30};
    int     cooldown_days{330};
    bool    reset_cooldown_on_restart{false};
    int     log_keep_count{5};
    int     preload_capacity{4};
    int     preload_workers{2};

    // ---- Timer System ----
    bool    timer_enabled{true};
    float   timer_x{0.94f}, timer_y{0.05f};
    int     timer_font_size{12};
    std::string timer_color{"yellow"};
    bool    filename_enabled{true};
    float   filename_x{0.04f}, filename_y{0.966f};
    bool    count_enabled{false};
    float   count_x{0.5f}, count_y{0.02f};
    int     videos_per_photos{10};
    int     video_volume{0};
    int     video_probe_timeout{3};
    bool    play_just_photos{false};
    bool    play_just_videos{false};
    bool    show_people_faces{true};
    bool    keep_animals{true};

    // New features v10.2.0
    bool    on_this_day_enabled{false};
    bool    web_dashboard_enabled{true};
    bool    diagnostics_hud_enabled{false};
    bool    adaptive_text_enabled{true};
    bool    twin_portrait_enabled{true};
    bool    closed_captions_enabled{true};

    // New features v10.3.1 (Balanced Interleaving & OS Fallbacks)
    std::string drm_connector{"auto"};
    std::string drm_card{"auto"};
    std::string font_path{"auto"};
    std::string video_audio_device{"auto"};

    // Subtitles
    std::string video_subtitles_dir{"/home/pi/piTrove/subtitles"};
    int     osd_offset_x{0};
    int     osd_offset_y{0};
    int     max_texture_dim{1920};
    int     http_socket_timeout{2};
    int     http_bind_attempts{10};

    // [slideshow] advanced
    std::string sleep_time{""};
    std::string wake_time{""};

    // [dashboard]
    bool    weather_enabled{false};
    float   weather_lat{-999.0f};
    float   weather_lon{-999.0f};

    // [remote]
    bool    http_enabled{false};
    int     http_port{8080};

    // [date_overlay]
    bool    date_overlay_enabled{false};
    std::string date_text{"%Y-%m-%d"};
    float   date_x{0.1f}, date_y{0.08f};
    int     date_font_size{20};
    std::string date_color{"cyan"};

    // [brightness]
    bool    brightness_auto{false};
    int     brightness_auto_min{50};
    int     brightness_auto_max{100};

    // [touch]
    bool    touch_enabled{false};

    // [collage]
    bool    collage_enabled{false};
    int     collage_cols{2};
    int     collage_rows{2};

    // [display] advanced
    bool    auto_display_rotation{false};

    // [display] border
    bool    border_enabled{true};
    int     border_width{10};
    bool    vignette_enabled{true};

    // [display] blurred background + color-matched matte
     bool    blurred_background{true};
     bool    color_matched_matte{true};
      std::string bg_style{"pattern"};
       int     pattern_offset{45};
       std::string pattern_style{"animated_combined"};
      int     blur_radius{14};
     int     glow_depth{43};
     bool    edge_glow_shadow{true};
    float    matte_opacity{0.50f};
     float   vignette_strength{0.35f};

     // Scale a base-pixel value (defined for 1080p) to current resolution
     int scale_px(int base_px_1080p) const {
         return (int)round((double)base_px_1080p * screen_w / 1920.0);
     }

    // [slideshow] extended
    bool    shuffle{true};
    float   ken_burns_zoom{0.15f};
    int     bias_strength{110};

    // [overlay] clock
    bool    clock_enabled{false};
    float   clock_x{0.5f}, clock_y{0.96f};
    int     clock_font_size{18};
    std::string clock_color{"white"};
    bool    clock_24h{true};

    // [overlay] font sizes
    int     filename_font_size{12};
    int     count_font_size{20};

    // [mqtt]
    bool        mqtt_enabled{false};
    std::string mqtt_broker{"192.168.4.111"};
    int         mqtt_port{1883};
    std::string mqtt_user{""};
    std::string mqtt_pass{""};
    std::string mqtt_topic_prefix{"piTrove"};
    std::string mqtt_motionsensor_topic{"home/motionsensor"};
    int         mqtt_motionsensor_cooldown{60};

    // [google_photos]
    bool        google_photos_enabled{false};
    std::string google_photos_client_id{""};
    std::string google_photos_client_secret{""};
    std::string google_photos_refresh_token{""};
    std::string google_photos_album_id{""};
    int         google_photos_sync_interval{60};
    std::string google_photos_cache_dir{"/app/cache/google_photos"};

    // [updates]
    bool        auto_update{false};
    std::string auto_update_branch{"main"};

    std::vector<std::string> ignore_folders;

    // CLI Overrides
    bool    dry_run{false};

    bool load(const std::string& path);
    bool save(const std::string& path);
    void parse_args(int argc, char** argv);
};

extern Config g_cfg;
extern std::mutex g_config_mtx;

#endif // PITROVE_CONFIG_H
