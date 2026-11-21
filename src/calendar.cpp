#include "calendar.h"
#include "http_transfer.h"
#include "config.h"
#include "config_text.h"
#include "infopanel_render.h"
#include "util.h"
#include "health.h"
#include <curl/curl.h>
#include <libical/ical.h>
#include <memory>
#include <set>
#include <algorithm>
#include <chrono>
#include <format>
#include <cstring>

GoogleCalendar::GoogleCalendar() {}

GoogleCalendar::~GoogleCalendar() {
    stop();
}



std::string GoogleCalendar::execute_http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        g_logger.error("CALENDAR: Failed to initialize libcurl");
        return "";
    }
    http_transfer::configure_public_https(curl);
    std::string response;
    http_transfer::TextBuffer response_buffer{&response, 4 * 1024 * 1024};

    // Cache-busting headers so Google FrontEnd (GFE) and intermediate proxies never serve stale feeds
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Cache-Control: no-cache, no-store, must-revalidate");
    headers = curl_slist_append(headers, "Pragma: no-cache");
    headers = curl_slist_append(headers, "Expires: 0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Append dynamic epoch timestamp parameter to URL to bust edge CDN caching
    std::string final_url = url;
    int64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    char sep = (final_url.find('?') == std::string::npos) ? '?' : '&';
    final_url += sep;
    final_url += "_t=" + std::to_string(now_ts);

    curl_easy_setopt(curl, CURLOPT_URL, final_url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_transfer::append_text);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "piTrove-18.0");

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (headers) {
        curl_slist_free_all(headers);
    }
    if (res != CURLE_OK || status != 200) {
        g_logger.warn("CALENDAR: HTTP request failed: curl={}, status={}", curl_easy_strerror(res), status);
        m_last_error.store(536);
        trigger_error(536);
        curl_easy_cleanup(curl);
        return "";
    }
    curl_easy_cleanup(curl);
    return response;
}

namespace {

const icaltimezone* feed_zone(icalcomponent* calendar, icalproperty* property) {
    if (!property) return nullptr;
    auto* parameter = icalproperty_get_first_parameter(property, ICAL_TZID_PARAMETER);
    if (!parameter) return nullptr;
    const char* id = icalparameter_get_tzid(parameter);
    if (!id) return nullptr;
    if (auto* timezone = icalcomponent_get_timezone(calendar, id)) return timezone;
    return icaltimezone_get_builtin_timezone(id);
}

time_t event_time(icaltimetype value, const std::string& display_tz, const char* tzid = nullptr) {
    using namespace std::chrono;
    if (icaltime_is_null_time(value) || !icaltime_is_valid_time(value)) return 0;
    if (icaltime_is_utc(value)) return icaltime_as_timet(value);
    if (value.zone && !tzid) return icaltime_as_timet_with_zone(value, value.zone);
    const year_month_day date{year{value.year}, month{unsigned(value.month)}, day{unsigned(value.day)}};
    if (!date.ok()) return 0;
    const auto local = local_days{date} + hours{value.hour} + minutes{value.minute} + seconds{value.second};
    try {
        const auto* zone = locate_zone(tzid ? tzid : display_tz);
        return zone->to_sys(local, choose::earliest).time_since_epoch().count();
    } catch (...) {
        if (value.zone) return icaltime_as_timet_with_zone(value, value.zone);
        return local.time_since_epoch().count();
    }
}

const char* timezone_id(icalproperty* property) {
    if (!property) return nullptr;
    auto* parameter = icalproperty_get_first_parameter(property, ICAL_TZID_PARAMETER);
    return parameter ? icalparameter_get_tzid(parameter) : nullptr;
}

std::string calendar_text(const char* value, std::string_view fallback = {}) {
    std::string result(value ? std::string_view(value) : fallback);
    for (char& c : result) if (static_cast<unsigned char>(c) < 32) c = ' ';
    return trim(result);
}

} // namespace

void GoogleCalendar::parse_ical(const std::string& ical_data) {
    const std::unique_ptr<icalcomponent, decltype(&icalcomponent_free)>
        calendar(icalparser_parse_string(ical_data.c_str()), icalcomponent_free);
    if (!calendar || icalcomponent_isa(calendar.get()) != ICAL_VCALENDAR_COMPONENT) {
        m_last_error.store(536);
        trigger_error(536);
        return;
    }

    std::string tz, name;
    size_t max_events;
    {
        std::shared_lock lock(g_config_mtx);
        tz = g_cfg.timezone.empty() ? "UTC" : g_cfg.timezone;
        name = g_cfg.gcalendar_name;
        max_events = std::clamp(g_cfg.gcalendar_max_events, 1, 16);
    }
    const time_t now = time(nullptr);
    constexpr time_t day = 86400;
    const time_t horizon = now + 366 * day;
    const auto today = format_epoch_tz(now, tz, "%Y-%m-%d");
    const auto tomorrow = format_tomorrow_tz(now, tz);
    const auto timestamp = [&](icalproperty* property, icaltimetype dt) {
        if (const auto* zone = feed_zone(calendar.get(), property)) dt.zone = zone;
        return event_time(dt, tz, timezone_id(property));
    };
    std::set<std::pair<std::string, time_t>> overrides;
    for (auto* event = icalcomponent_get_first_component(calendar.get(), ICAL_VEVENT_COMPONENT);
         event; event = icalcomponent_get_next_component(calendar.get(), ICAL_VEVENT_COMPONENT)) {
        if (auto* id = icalcomponent_get_first_property(event, ICAL_RECURRENCEID_PROPERTY)) {
            if (const char* uid = icalcomponent_get_uid(event))
                overrides.emplace(uid, timestamp(id, icalproperty_get_recurrenceid(id)));
        }
    }

    std::vector<CalendarEvent> upcoming;
    size_t iteration_budget = 100000;
    for (auto* event = icalcomponent_get_first_component(calendar.get(), ICAL_VEVENT_COMPONENT);
         event; event = icalcomponent_get_next_component(calendar.get(), ICAL_VEVENT_COMPONENT)) {
        if (icalcomponent_get_status(event) == ICAL_STATUS_CANCELLED) continue;
        auto* start_property = icalcomponent_get_first_property(event, ICAL_DTSTART_PROPERTY);
        if (!start_property) continue;
        auto start = icalproperty_get_dtstart(start_property);
        if (const auto* zone = feed_zone(calendar.get(), start_property)) start.zone = zone;
        const char* start_tzid = timezone_id(start_property);
        const auto base_time = event_time(start, tz, start_tzid);
        if (!base_time) continue;
        const auto* uid = icalcomponent_get_uid(event);
        const bool detached = icalcomponent_get_first_property(event, ICAL_RECURRENCEID_PROPERTY) != nullptr;
        const bool all_day = start.is_date;
        const auto* summary = icalcomponent_get_summary(event);
        const auto* location = icalcomponent_get_location(event);

        int duration = all_day ? day : 0;
        if (auto* end_property = icalcomponent_get_first_property(event, ICAL_DTEND_PROPERTY)) {
            const auto end = icalproperty_get_dtend(end_property);
            if (all_day && end.is_date) {
                auto utc_start = start;
                auto utc_end = end;
                utc_start.zone = utc_end.zone = icaltimezone_get_utc_timezone();
                duration = std::clamp<time_t>(icaltime_as_timet(utc_end) - icaltime_as_timet(utc_start), day, 366 * day);
            } else {
                duration = std::clamp<time_t>(timestamp(end_property, end) - base_time, 0, 366 * day);
            }
        } else if (auto* property = icalcomponent_get_first_property(event, ICAL_DURATION_PROPERTY)) {
            duration = std::clamp<time_t>(icaldurationtype_as_int(icalproperty_get_duration(property)), 0, 366 * day);
        }

        std::set<time_t> excluded, seen;
        for (auto* date = icalcomponent_get_first_property(event, ICAL_EXDATE_PROPERTY);
             date; date = icalcomponent_get_next_property(event, ICAL_EXDATE_PROPERTY)) {
            auto value = icalproperty_get_exdate(date);
            if (!timezone_id(date) && !icaltime_is_utc(value)) {
                value.zone = start.zone;
                excluded.insert(event_time(value, tz, start_tzid));
            } else excluded.insert(timestamp(date, value));
        }

        const auto add = [&](icaltimetype instance, time_t begin, int span) {
            if (!begin || begin > horizon || excluded.contains(begin) ||
                (!detached && uid && overrides.contains({uid, begin}))) return;
            const auto end = all_day ? event_time(icaltime_add(instance, icaldurationtype_from_int(span)), tz, start_tzid)
                                     : begin + span;
            const auto cutoff = (end > begin ? end : begin + 3600) + 3600;
            if (cutoff < now || !seen.insert(begin).second ||
                (upcoming.size() == max_events && begin >= upcoming.back().start_time)) return;

            CalendarEvent item;
            item.summary = calendar_text(summary, "Calendar Event");
            item.location = calendar_text(location);
            item.start_time = begin;
            item.end_time = end;
            item.cutoff_time = cutoff;
            item.all_day = all_day;
            item.calendar_name = name;
            const auto date = format_epoch_tz(begin, tz, "%Y-%m-%d");
            item.formatted_date = format_epoch_tz(begin, tz, "%a, %b %d");
            item.relative_day = date == today ? "TODAY" : date == tomorrow ? "TOMORROW" : item.formatted_date;
            item.formatted_time = all_day ? "ALL DAY" : format_epoch_tz(begin, tz, "%I:%M %p");
            if (!all_day && end > begin) item.formatted_time += " - " + format_epoch_tz(end, tz, "%I:%M %p");
            const auto position = std::upper_bound(upcoming.begin(), upcoming.end(), begin,
                [](time_t t, const CalendarEvent& entry) { return t < entry.start_time; });
            upcoming.insert(position, std::move(item));
            if (upcoming.size() > max_events) upcoming.pop_back();
        };

        add(start, base_time, duration);
        if (detached) continue;
        for (auto* rule = icalcomponent_get_first_property(event, ICAL_RRULE_PROPERTY);
             rule && iteration_budget; rule = icalcomponent_get_next_property(event, ICAL_RRULE_PROPERTY)) {
            const auto recurrence = icalproperty_get_rrule(rule);
            const std::unique_ptr<icalrecur_iterator, decltype(&icalrecur_iterator_free)>
                iterator(icalrecur_iterator_new(recurrence, start), icalrecur_iterator_free);
            if (!iterator) continue;
            if (!recurrence.count && base_time < now - duration - 7200) {
                // Start two days early so timezone transitions cannot hide ongoing events.
                const auto earliest = icaltime_from_timet_with_zone(now - duration - 2 * day, start.is_date,
                    start.zone ? start.zone : icaltimezone_get_utc_timezone());
                auto first = earliest;
                first.zone = start.zone;
                icalrecur_iterator_set_start(iterator.get(), first);
            }
            for (size_t attempts = 0; attempts < 10000 && iteration_budget > 0; ++attempts) {
                --iteration_budget;
                const auto instance = icalrecur_iterator_next(iterator.get());
                if (icaltime_is_null_time(instance)) break;
                const auto begin = event_time(instance, tz, start_tzid);
                if (begin > horizon || (upcoming.size() == max_events && begin >= upcoming.back().start_time)) break;
                add(instance, begin, duration);
            }
        }
        for (auto* date = icalcomponent_get_first_property(event, ICAL_RDATE_PROPERTY);
             date; date = icalcomponent_get_next_property(event, ICAL_RDATE_PROPERTY)) {
            const auto value = icalproperty_get_rdate(date);
            auto instance = icaltime_is_null_time(value.time) ? value.period.start : value.time;
            if (icaltime_is_null_time(instance)) continue;
            if (!timezone_id(date) && !icaltime_is_utc(instance)) instance.zone = start.zone;
            const auto begin = timezone_id(date) ? timestamp(date, instance) : event_time(instance, tz, start_tzid);
            int span = duration;
            if (!icaltime_is_null_time(value.period.end))
                span = std::clamp<time_t>(timestamp(date, value.period.end) - begin, 0, 366 * day);
            else if (!icaldurationtype_is_null_duration(value.period.duration))
                span = std::clamp<time_t>(icaldurationtype_as_int(value.period.duration), 0, 366 * day);
            add(instance, begin, span);
        }
    }

    {
        std::unique_lock lock(m_events_mtx);
        m_events = std::move(upcoming);
        m_last_error.store(0);
        m_last_sync_time.store(now);
    }
    clear_error(536);
}

void GoogleCalendar::sync() {
    if (m_fetching.exchange(true)) return;
    struct Guard { std::atomic<bool>& f; ~Guard() { f.store(false); } } g{m_fetching};

    std::string ical_url;
    std::string calendar_name;
    {
        std::shared_lock lk(g_config_mtx);
        ical_url = g_cfg.gcalendar_ical_url;
        calendar_name = g_cfg.gcalendar_name;
    }

    if (ical_url.empty()) {
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
        // Never log the full URL: a Google private iCal URL *is* the credential
        // (the private-<hex> path segment grants full read access on its own),
        // and this line previously wrote it to disk in plaintext on every sync.
        g_logger.info("CALENDAR: Syncing Google Calendar feed '{}' ({} chars)",
                      calendar_name.empty() ? "ical" : calendar_name.c_str(),
                      ical_url.size());
        std::string ical_data = execute_http_get(ical_url);
        if (!ical_data.empty()) {
            parse_ical(ical_data);
        }
    }
}

bool GoogleCalendar::start() {
    std::lock_guard lifecycle(m_lifecycle_mtx);
    m_running.store(false);
    if (m_worker_thread.joinable()) m_worker_thread.join();
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
    std::lock_guard lifecycle(m_lifecycle_mtx);
    m_running.store(false);
    if (m_worker_thread.joinable()) m_worker_thread.join();
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
        ss << "{\"summary\":\"" << config_text::escape(m_events[i].summary) << "\""
           << ",\"location\":\"" << config_text::escape(m_events[i].location) << "\""
           << ",\"relative_day\":\"" << m_events[i].relative_day << "\""
           << ",\"time\":\"" << m_events[i].formatted_time << "\""
           << ",\"all_day\":" << (m_events[i].all_day ? "true" : "false") << "}";
    }
    ss << "]}";
    return ss.str();
}

static std::vector<std::string> wrap_text_to_width(FontRenderer* font_renderer, const FontHandle& font, const std::string& text, int max_w) {
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
            lines.push_back(font_renderer->ellipsize(font, current_line, max_w));
            current_line = word;
        }
    }
    if (!current_line.empty()) {
        lines.push_back(font_renderer->ellipsize(font, current_line, max_w));
    }
    return lines;
}

void GoogleCalendar::render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w) {
    if (!renderer || !font_renderer || bounds.h <= 0 || bounds.w <= 0) return;

    const InfopanelClip clip(renderer, bounds);
    if (!clip) return;

    // 1. Card backdrop
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

    const auto px = [screen_w](int value) { return panel_px(value, screen_w); };
    const int title_font_size = px(panel_heading_size);
    const int body_font_size = px(panel_body_size);
    const int sub_font_size = px(panel_detail_size);

    const auto title_font_owner = font_renderer->load_font(font_path, title_font_size);
    FontHandle& title_font = *title_font_owner;
    const auto body_font_owner = font_renderer->load_font(font_path, body_font_size);
    FontHandle& body_font = *body_font_owner;
    const auto sub_font_owner = font_renderer->load_font(font_path, sub_font_size);
    FontHandle& sub_font = *sub_font_owner;

    const int inner_width = bounds.w - 2 * px(18);
    if (inner_width <= 0) return;
    const int pad_x = bounds.x + px(18);
    int cur_y = bounds.y + (has_matting ? (int)round(((double)mat_size * 0.5) + 10.0) : (int)round(20.0 * screen_w / 1920.0));

    // Header title
    font_renderer->draw_text(pad_x, cur_y, title_font, font_renderer->ellipsize(title_font, day_str, inner_width), 255, 255, 255, 255);
    cur_y += TTF_GetFontHeight(title_font.font) + px(4);

    std::string header_sub = std::format("{} • {}", date_str, cal_name.empty() ? "Calendar" : cal_name);
    font_renderer->draw_text(pad_x, cur_y, sub_font, font_renderer->ellipsize(sub_font, header_sub, inner_width), 0, 200, 255, 220);
    cur_y += TTF_GetFontHeight(sub_font.font) + px(18);

    // Header divider line
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 30);
    SDL_FRect div_line = { (float)pad_x, (float)cur_y, (float)(bounds.w - (pad_x - bounds.x) * 2), 1.0f };
    SDL_RenderFillRect(renderer, &div_line);
    cur_y += px(18);

    // 3. Render event cards
    std::vector<CalendarEvent> events_copy;
    {
        std::shared_lock lk(m_events_mtx);
        events_copy = m_events;
    }

    // Dynamic real-time pruning and relative day recalculation on render
    std::string today_date = format_epoch_tz(now, tz, "%Y-%m-%d");
    std::string tomorrow_date = format_tomorrow_tz(now, tz);

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
        font_renderer->draw_text(pad_x, cur_y, body_font,
                                 font_renderer->ellipsize(body_font, "No upcoming events scheduled", inner_width), 160, 175, 195, 255);
        cur_y += TTF_GetFontHeight(body_font.font) + px(8);
        font_renderer->draw_text(pad_x, cur_y, sub_font,
                                 font_renderer->ellipsize(sub_font, "Add events to Google Calendar to sync", inner_width), 110, 125, 145, 255);
        return;
    }

    const int pill_w = px(3);
    const int text_start_x = pad_x + pill_w + px(10);
    const int text_max_w = inner_width - (text_start_x - pad_x);
    if (text_max_w <= 0) return;
    const int tag_height = TTF_GetFontHeight(sub_font.font) + px(4);
    const int summary_height = TTF_GetFontHeight(body_font.font) + px(3);
    const int location_height = TTF_GetFontHeight(sub_font.font) + px(3);

    for (const auto& ev : active_events) {
        std::vector<std::string> summary_lines = wrap_text_to_width(font_renderer, body_font, ev.summary, text_max_w);
        std::vector<std::string> loc_lines;
        if (!ev.location.empty()) {
            loc_lines = wrap_text_to_width(font_renderer, sub_font, "@ " + ev.location, text_max_w);
        }

        const int total_event_h = tag_height + int(summary_lines.size()) * summary_height +
                                  int(loc_lines.size()) * location_height;
        if (cur_y + total_event_h > bounds.y + bounds.h - px(10)) break;

        std::string tag_str = std::format("{} • {}", ev.relative_day, ev.formatted_time);
        uint8_t tr = 0, tg = 200, tb = 255;
        if (ev.relative_day == "TODAY") {
            tr = 255; tg = 180; tb = 50;
        }

        // Left accent indicator pill matching multi-line card height
        int pill_h = total_event_h - 2;
        SDL_FRect pill_rect = { (float)pad_x, (float)cur_y, (float)pill_w, (float)pill_h };
        SDL_SetRenderDrawColor(renderer, tr, tg, tb, 255);
        SDL_RenderFillRect(renderer, &pill_rect);

        font_renderer->draw_text(text_start_x, cur_y, sub_font,
                                 font_renderer->ellipsize(sub_font, tag_str, text_max_w), tr, tg, tb, 255);
        cur_y += tag_height;

        // Wrapped event summary lines
        for (const auto& line : summary_lines) {
            font_renderer->draw_text(text_start_x, cur_y, body_font, line, 245, 250, 255, 255);
            cur_y += summary_height;
        }

        // Wrapped location lines if present
        for (const auto& line : loc_lines) {
            font_renderer->draw_text(text_start_x, cur_y, sub_font, line, 140, 160, 180, 200);
            cur_y += location_height;
        }

        cur_y += px(10); // Spacing between events
    }
}
