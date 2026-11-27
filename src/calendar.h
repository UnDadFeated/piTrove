#ifndef PITROVE_CALENDAR_H
#define PITROVE_CALENDAR_H

#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <optional>
#include <thread>
#include <atomic>
#include "font_render.h"

struct CalendarEvent {
    std::string summary;
    std::string location;
    time_t start_time{0};
    time_t end_time{0};
    time_t cutoff_time{0};
    bool all_day{false};
    std::string calendar_name;
    std::string formatted_date;
    std::string formatted_time;
    std::string relative_day; // Today, Tomorrow, Wed Aug 26
    std::string color_hex;
};

class GoogleCalendar {
private:
    std::vector<CalendarEvent> m_events;
    mutable std::shared_mutex m_events_mtx;
    uint64_t m_events_generation = 0;

    // Render-thread-owned snapshot and layout.
    struct WrappedEvent {
        std::vector<std::string> summary;
        std::vector<std::string> location;
    };
    std::vector<CalendarEvent> m_render_events;
    std::vector<WrappedEvent> m_wrapped_events;
    uint64_t m_render_generation = 0;
    int m_wrapped_width = 0;
    std::weak_ptr<FontHandle> m_wrapped_body_font, m_wrapped_sub_font;
    std::jthread m_worker_thread;
    std::mutex m_lifecycle_mtx;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_allow_io{true};
    std::mutex m_fetch_mtx;
    uint64_t m_fetch_generation = 0;
    bool m_fetching = false;
    void refresh(bool requested);
    void fetch_once(uint64_t generation);
    std::atomic<int> m_last_error{0};
    std::atomic<time_t> m_last_sync_time{0};

    std::string execute_http_get(const std::string& url);
    std::optional<std::vector<CalendarEvent>> parse_ical(const std::string& ical_data, const std::string& tz,
                                                         const std::string& name, size_t max_events);

public:
    GoogleCalendar();
    ~GoogleCalendar();

    bool is_running() const { return m_running.load(); }
    bool start();
    void stop();

    void sync();
    std::vector<CalendarEvent> get_events() const;
    std::string get_status_json() const;
    int get_last_error() const { return m_last_error.load(); }

    void render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w);
};

inline GoogleCalendar g_calendar;

#endif // PITROVE_CALENDAR_H
