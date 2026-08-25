#ifndef PITROVE_CALENDAR_H
#define PITROVE_CALENDAR_H

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include "font_render.h"

struct CalendarEvent {
    std::string summary;
    std::string location;
    time_t start_time{0};
    time_t end_time{0};
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
    std::jthread m_worker_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_fetching{false};
    std::atomic<int> m_last_error{0};
    std::atomic<time_t> m_last_sync_time{0};

    std::string execute_http_get(const std::string& url);
    void parse_ical(const std::string& ical_data);

public:
    GoogleCalendar();
    ~GoogleCalendar();

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
