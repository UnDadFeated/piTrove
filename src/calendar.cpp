#include "calendar.h"
#include "config.h"
#include "util.h"
#include "health.h"
#include <curl/curl.h>
#include <regex>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <format>
#include <cstring>

GoogleCalendar::GoogleCalendar() {}

GoogleCalendar::~GoogleCalendar() {
    stop();
}

static size_t cal_curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string GoogleCalendar::execute_http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        g_logger.error("CALENDAR: Failed to initialize libcurl");
        return "";
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cal_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "piTrove-18.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        g_logger.warn("CALENDAR: HTTP request failed: {}", curl_easy_strerror(res));
        m_last_error.store(536);
        trigger_error(536);
        curl_easy_cleanup(curl);
        return "";
    }
    curl_easy_cleanup(curl);
    return response;
}

static time_t parse_ical_datetime_tz(const std::string& dt_str, const std::string& tz_name, bool& out_all_day) {
    if (dt_str.empty()) return 0;
    std::string zone = tz_name.empty() ? "UTC" : tz_name;

    // All-day: YYYYMMDD (no 'T')
    if (dt_str.find('T') == std::string::npos) {
        out_all_day = true;
        if (dt_str.size() < 8) return 0;
        try {
            int y = std::stoi(dt_str.substr(0, 4));
            int m = std::stoi(dt_str.substr(4, 2));
            int d = std::stoi(dt_str.substr(6, 2));
            std::chrono::year_month_day ymd{std::chrono::year{y}, std::chrono::month{(unsigned)m}, std::chrono::day{(unsigned)d}};
            auto local_tp = std::chrono::local_days{ymd} + std::chrono::hours{0};
            auto zt = std::chrono::zoned_time{zone, local_tp};
            return std::chrono::system_clock::to_time_t(zt.get_sys_time());
        } catch (...) {
            return 0;
        }
    }

    // Timed appointment
    out_all_day = false;
    if (dt_str.back() == 'Z') {
        struct tm tm_buf;
        std::memset(&tm_buf, 0, sizeof(tm_buf));
        if (strptime(dt_str.c_str(), "%Y%m%dT%H%M%SZ", &tm_buf) != nullptr) {
            return timegm(&tm_buf);
        }
    } else {
        if (dt_str.size() < 13) return 0;
        try {
            int y = std::stoi(dt_str.substr(0, 4));
            int m = std::stoi(dt_str.substr(4, 2));
            int d = std::stoi(dt_str.substr(6, 2));
            int hh = std::stoi(dt_str.substr(9, 2));
            int mm = std::stoi(dt_str.substr(11, 2));
            int ss = (dt_str.size() >= 15) ? std::stoi(dt_str.substr(13, 2)) : 0;
            std::chrono::year_month_day ymd{std::chrono::year{y}, std::chrono::month{(unsigned)m}, std::chrono::day{(unsigned)d}};
            auto local_tp = std::chrono::local_days{ymd} + std::chrono::hours{hh} + std::chrono::minutes{mm} + std::chrono::seconds{ss};
            auto zt = std::chrono::zoned_time{zone, local_tp};
            return std::chrono::system_clock::to_time_t(zt.get_sys_time());
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

static std::string unescape_ical_text(std::string text) {
    auto replace_all = [](std::string& str, const std::string& from, const std::string& to) {
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    };
    replace_all(text, "\\,", ",");
    replace_all(text, "\\;", ";");
    replace_all(text, "\\n", " ");
    replace_all(text, "\\N", " ");
    replace_all(text, "\\\\", "\\");
    return trim(text);
}

void GoogleCalendar::parse_ical(const std::string& ical_data) {
    if (ical_data.empty()) return;

    // 1. Unfold lines (lines beginning with space or tab are continuation)
    std::string unfolded;
    unfolded.reserve(ical_data.size());
    std::istringstream stream(ical_data);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            if (!unfolded.empty()) unfolded.append(line.substr(1));
        } else {
            if (!unfolded.empty()) unfolded.push_back('\n');
            unfolded.append(line);
        }
    }

    std::string tz = "UTC";
    std::string cal_filter = "Family";
    int max_events = 8;
    {
        std::shared_lock lk(g_config_mtx);
        tz = g_cfg.timezone;
        cal_filter = g_cfg.gcalendar_name;
        max_events = std::clamp(g_cfg.gcalendar_max_events, 1, 16);
    }

    time_t now = time(nullptr);

    std::regex event_regex("BEGIN:VEVENT([\\s\\S]*?)END:VEVENT");
    std::regex summary_regex("SUMMARY:(.*)");
    std::regex dtstart_regex("DTSTART(?:;[^:]+)?:([0-9TZ]+)");
    std::regex dtend_regex("DTEND(?:;[^:]+)?:([0-9TZ]+)");
    std::regex loc_regex("LOCATION:(.*)");

    std::vector<CalendarEvent> parsed_events;
    auto events_begin = std::sregex_iterator(unfolded.begin(), unfolded.end(), event_regex);
    auto events_end = std::sregex_iterator();

    for (std::sregex_iterator i = events_begin; i != events_end; ++i) {
        std::string ev_block = (*i)[1].str();
        std::smatch m;

        std::string raw_summary, raw_dtstart, raw_dtend, raw_location;
        if (std::regex_search(ev_block, m, summary_regex)) raw_summary = m[1].str();
        if (std::regex_search(ev_block, m, dtstart_regex)) raw_dtstart = m[1].str();
        if (std::regex_search(ev_block, m, dtend_regex)) raw_dtend = m[1].str();
        if (std::regex_search(ev_block, m, loc_regex)) raw_location = m[1].str();

        bool all_day = false;
        time_t start_t = parse_ical_datetime_tz(raw_dtstart, tz, all_day);
        bool dummy = false;
        time_t end_t = parse_ical_datetime_tz(raw_dtend, tz, dummy);
        if (start_t == 0) continue;

        // Expiration Cutoff Calculation:
        // - All-day events expire at 1:00 AM on the day following the event (start_t + 24h + 1h)
        // - Timed appointments expire 1 hour after the appointment ends
        time_t cutoff_t = 0;
        if (all_day) {
            cutoff_t = start_t + 86400 + 3600;
        } else {
            time_t effective_end = (end_t > start_t) ? end_t : (start_t + 3600);
            cutoff_t = effective_end + 3600;
        }

        // Skip events that have already passed their 1-hour expiration cutoff
        if (now > cutoff_t) continue;

        CalendarEvent ev;
        ev.summary = unescape_ical_text(raw_summary.empty() ? "Calendar Event" : raw_summary);
        ev.location = unescape_ical_text(raw_location);
        ev.start_time = start_t;
        ev.end_time = end_t;
        ev.cutoff_time = cutoff_t;
        ev.all_day = all_day;
        ev.calendar_name = cal_filter;

        // Compute relative day string in target timezone
        std::string ev_date = format_epoch_tz(start_t, tz, "%Y-%m-%d");
        std::string today_date = format_epoch_tz(now, tz, "%Y-%m-%d");
        std::string tomorrow_date = format_epoch_tz(now + 86400, tz, "%Y-%m-%d");

        if (ev_date == today_date) {
            ev.relative_day = "TODAY";
        } else if (ev_date == tomorrow_date) {
            ev.relative_day = "TOMORROW";
        } else {
            ev.relative_day = format_epoch_tz(start_t, tz, "%a, %b %d");
        }

        if (all_day) {
            ev.formatted_time = "ALL DAY";
        } else {
            std::string start_str = format_epoch_tz(start_t, tz, "%I:%M %p");
            if (end_t > start_t) {
                std::string end_str = format_epoch_tz(end_t, tz, "%I:%M %p");
                ev.formatted_time = std::format("{} - {}", start_str, end_str);
            } else {
                ev.formatted_time = start_str;
            }
        }

        ev.formatted_date = format_epoch_tz(start_t, tz, "%a, %b %d");
        parsed_events.push_back(std::move(ev));
    }

    // Sort chronologically
    std::sort(parsed_events.begin(), parsed_events.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
        return a.start_time < b.start_time;
    });

    if (parsed_events.size() > (size_t)max_events) {
        parsed_events.resize(max_events);
    }

    {
        std::unique_lock lk(m_events_mtx);
        m_events = std::move(parsed_events);
        m_last_error.store(0);
        m_last_sync_time.store(now);
        g_logger.info("CALENDAR: Synced {} upcoming events for calendar '{}' in timezone {}", m_events.size(), cal_filter, tz);
    }
}

void GoogleCalendar::sync() {
    if (m_fetching.exchange(true)) return;
    struct Guard { std::atomic<bool>& f; ~Guard() { f.store(false); } } g{m_fetching};

    std::string ical_url;
    std::string api_key;
    {
        std::shared_lock lk(g_config_mtx);
        ical_url = g_cfg.gcalendar_ical_url;
        api_key = g_cfg.gcalendar_api_key;
    }

    if (ical_url.empty() && api_key.empty()) {
        g_logger.info("CALENDAR: No iCal URL configured, providing default preview agenda");
        time_t now = time(nullptr);
        std::string tz = "UTC";
        { std::shared_lock lk(g_config_mtx); tz = g_cfg.timezone; }

        std::vector<CalendarEvent> sample_events;
        CalendarEvent e1;
        e1.summary = "Family Dinner";
        e1.location = "Home";
        e1.start_time = now + 3600 * 2;
        e1.end_time = now + 3600 * 4;
        e1.all_day = false;
        e1.relative_day = "TODAY";
        e1.formatted_time = format_epoch_tz(e1.start_time, tz, "%I:%M %p") + " - " + format_epoch_tz(e1.end_time, tz, "%I:%M %p");
        sample_events.push_back(e1);

        CalendarEvent e2;
        e2.summary = "Soccer Practice";
        e2.location = "Community Park";
        e2.start_time = now + 86400 + 3600 * 3;
        e2.end_time = now + 86400 + 3600 * 5;
        e2.all_day = false;
        e2.relative_day = "TOMORROW";
        e2.formatted_time = format_epoch_tz(e2.start_time, tz, "%I:%M %p") + " - " + format_epoch_tz(e2.end_time, tz, "%I:%M %p");
        sample_events.push_back(e2);

        CalendarEvent e3;
        e3.summary = "School Orientation";
        e3.location = "Main Auditorium";
        e3.start_time = now + 86400 * 2;
        e3.end_time = now + 86400 * 2;
        e3.all_day = true;
        e3.relative_day = format_epoch_tz(e3.start_time, tz, "%a, %b %d");
        e3.formatted_time = "ALL DAY";
        sample_events.push_back(e3);

        {
            std::unique_lock lk(m_events_mtx);
            m_events = std::move(sample_events);
            m_last_sync_time.store(now);
        }
        return;
    }

    if (!ical_url.empty()) {
        g_logger.info("CALENDAR: Syncing Google Calendar feed from {}", ical_url);
        std::string ical_data = execute_http_get(ical_url);
        if (!ical_data.empty()) {
            parse_ical(ical_data);
        }
    }
}

bool GoogleCalendar::start() {
    stop();
    m_running.store(true);

    if (!spawn_thread_safe(m_worker_thread, "gcalendar", [this]() {
        this->sync();

        while (this->m_running.load()) {
            int refresh_mins = 15;
            {
                std::shared_lock lk(g_config_mtx);
                refresh_mins = std::clamp(g_cfg.gcalendar_refresh_minutes, 5, 120);
            }
            for (int i = 0; i < refresh_mins * 60 && this->m_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (this->m_running.load()) {
                this->sync();
            }
        }
    })) {
        g_logger.error("CALENDAR: Failed to spawn background worker thread");
        m_running.store(false);
        return false;
    }
    return true;
}

void GoogleCalendar::stop() {
    m_running.store(false);
}

std::vector<CalendarEvent> GoogleCalendar::get_events() const {
    std::shared_lock lk(m_events_mtx);
    return m_events;
}

std::string GoogleCalendar::get_status_json() const {
    std::shared_lock lk(m_events_mtx);
    std::ostringstream ss;
    ss << "{\"count\":" << m_events.size()
       << ",\"last_sync\":" << m_last_sync_time.load()
       << ",\"last_error\":" << m_last_error.load()
       << ",\"events\":[";
    for (size_t i = 0; i < m_events.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"summary\":\"" << escape_shell_arg(m_events[i].summary) << "\""
           << ",\"location\":\"" << escape_shell_arg(m_events[i].location) << "\""
           << ",\"relative_day\":\"" << m_events[i].relative_day << "\""
           << ",\"time\":\"" << m_events[i].formatted_time << "\""
           << ",\"all_day\":" << (m_events[i].all_day ? "true" : "false") << "}";
    }
    ss << "]}";
    return ss.str();
}

static std::vector<std::string> wrap_text_to_width(FontRenderer* font_renderer, FontHandle& font, const std::string& text, int max_w) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;

    std::istringstream stream(text);
    std::string word;
    std::string current_line;

    while (stream >> word) {
        std::string test_line = current_line.empty() ? word : (current_line + " " + word);
        int tw = 0, th = 0;
        font_renderer->measure(font, test_line, tw, th);
        if (tw <= max_w || current_line.empty()) {
            current_line = test_line;
        } else {
            lines.push_back(current_line);
            current_line = word;
        }
    }
    if (!current_line.empty()) {
        lines.push_back(current_line);
    }
    return lines;
}

void GoogleCalendar::render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w) {
    if (!renderer || !font_renderer || bounds.h <= 0 || bounds.w <= 0) return;

    // 1. Glassmorphic card backdrop
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 14, 18, 28, 235); // Sleek dark glass card
    SDL_FRect bg_rect = { (float)bounds.x, (float)bounds.y, (float)bounds.w, (float)bounds.h };
    SDL_RenderFillRect(renderer, &bg_rect);

    // Left divider accent border
    SDL_SetRenderDrawColor(renderer, 0, 200, 255, 140);
    SDL_FRect left_border = { (float)bounds.x, (float)bounds.y, 2.0f, (float)bounds.h };
    SDL_RenderFillRect(renderer, &left_border);

    // 2. Header: Current Day & Date in configured Timezone
    std::string tz = "UTC";
    std::string cal_name = "Family";
    bool has_matting = false;
    int mat_size = 0;
    {
        std::shared_lock lk(g_config_mtx);
        tz = g_cfg.timezone;
        cal_name = g_cfg.gcalendar_name;
        has_matting = g_cfg.matting;
        mat_size = (int)round((double)g_cfg.matting_size * screen_w / 1920.0);
    }

    time_t now = time(nullptr);
    std::string day_str = format_epoch_tz(now, tz, "%A");
    std::string date_str = format_epoch_tz(now, tz, "%B %d");

    int title_font_size = (int)round(22.0 * screen_w / 1920.0);
    int body_font_size = (int)round(14.0 * screen_w / 1920.0);
    int sub_font_size = (int)round(12.0 * screen_w / 1920.0);

    FontHandle& title_font = font_renderer->load_font(font_path, title_font_size);
    FontHandle& body_font = font_renderer->load_font(font_path, body_font_size);
    FontHandle& sub_font = font_renderer->load_font(font_path, sub_font_size);

    int pad_x = bounds.x + (int)round(18.0 * screen_w / 1920.0);
    int cur_y = bounds.y + (has_matting ? (int)round(((double)mat_size * 0.5) + 10.0) : (int)round(20.0 * screen_w / 1920.0));

    // Header title
    font_renderer->draw_text(pad_x, cur_y, title_font, day_str, 255, 255, 255, 255);
    cur_y += title_font_size + 4;

    std::string header_sub = std::format("{} • {}", date_str, cal_name.empty() ? "Calendar" : cal_name);
    font_renderer->draw_text(pad_x, cur_y, sub_font, header_sub, 0, 200, 255, 220);
    cur_y += sub_font_size + 18;

    // Header divider line
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 30);
    SDL_FRect div_line = { (float)pad_x, (float)cur_y, (float)(bounds.w - (pad_x - bounds.x) * 2), 1.0f };
    SDL_RenderFillRect(renderer, &div_line);
    cur_y += 18;

    // 3. Render event cards
    std::vector<CalendarEvent> events_copy;
    {
        std::shared_lock lk(m_events_mtx);
        events_copy = m_events;
    }

    // Dynamic real-time pruning and relative day recalculation on render
    std::string today_date = format_epoch_tz(now, tz, "%Y-%m-%d");
    std::string tomorrow_date = format_epoch_tz(now + 86400, tz, "%Y-%m-%d");

    std::vector<CalendarEvent> active_events;
    for (auto ev : events_copy) {
        if (ev.cutoff_time > 0 && now > ev.cutoff_time) continue;

        std::string ev_date = format_epoch_tz(ev.start_time, tz, "%Y-%m-%d");
        if (ev_date == today_date) {
            ev.relative_day = "TODAY";
        } else if (ev_date == tomorrow_date) {
            ev.relative_day = "TOMORROW";
        } else {
            ev.relative_day = format_epoch_tz(ev.start_time, tz, "%a, %b %d");
        }
        active_events.push_back(std::move(ev));
    }

    if (active_events.empty()) {
        font_renderer->draw_text(pad_x, cur_y, body_font, "No upcoming events scheduled", 160, 175, 195, 255);
        cur_y += body_font_size + 8;
        font_renderer->draw_text(pad_x, cur_y, sub_font, "Add events to Google Calendar to sync", 110, 125, 145, 255);
        return;
    }

    int right_matte_limit = screen_w - (int)round(60.0 * screen_w / 1920.0);
    int card_right_limit = bounds.x + bounds.w - (int)round(18.0 * screen_w / 1920.0);
    int text_max_w = std::min(right_matte_limit, card_right_limit) - (pad_x + (int)round(3.0 * screen_w / 1920.0) + 10);
    if (text_max_w < 120) text_max_w = 240;

    for (const auto& ev : active_events) {
        std::vector<std::string> summary_lines = wrap_text_to_width(font_renderer, body_font, ev.summary, text_max_w);
        std::vector<std::string> loc_lines;
        if (!ev.location.empty()) {
            loc_lines = wrap_text_to_width(font_renderer, sub_font, "@ " + ev.location, text_max_w);
        }

        int total_event_h = (sub_font_size + 4) 
                          + (int)summary_lines.size() * (body_font_size + 3)
                          + (int)loc_lines.size() * (sub_font_size + 3);

        if (cur_y + total_event_h > bounds.y + bounds.h - 10) break; // Keep within card bounds

        std::string tag_str = std::format("{} • {}", ev.relative_day, ev.formatted_time);
        uint8_t tr = 0, tg = 200, tb = 255;
        if (ev.relative_day == "TODAY") {
            tr = 255; tg = 180; tb = 50;
        }

        // Left accent indicator pill matching multi-line card height
        int pill_w = (int)round(3.0 * screen_w / 1920.0);
        int pill_h = total_event_h - 2;
        SDL_FRect pill_rect = { (float)pad_x, (float)cur_y, (float)pill_w, (float)pill_h };
        SDL_SetRenderDrawColor(renderer, tr, tg, tb, 255);
        SDL_RenderFillRect(renderer, &pill_rect);

        int text_start_x = pad_x + pill_w + 10;
        font_renderer->draw_text(text_start_x, cur_y, sub_font, tag_str, tr, tg, tb, 255);
        cur_y += sub_font_size + 4;

        // Wrapped event summary lines
        for (const auto& line : summary_lines) {
            font_renderer->draw_text(text_start_x, cur_y, body_font, line, 245, 250, 255, 255);
            cur_y += body_font_size + 3;
        }

        // Wrapped location lines if present
        for (const auto& line : loc_lines) {
            font_renderer->draw_text(text_start_x, cur_y, sub_font, line, 140, 160, 180, 200);
            cur_y += sub_font_size + 3;
        }

        cur_y += 10; // Spacing between events
    }
}
