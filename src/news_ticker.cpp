#include "news_ticker.h"
#include "config.h"
#include "util.h"
#include "health.h"
#include <curl/curl.h>
#include <regex>
#include <sstream>
#include <chrono>
#include <format>
#include <cstring>

NewsTicker::NewsTicker() {}

NewsTicker::~NewsTicker() {
    stop();
}

static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string NewsTicker::execute_http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        g_logger.error("NEWS: Failed to initialize libcurl");
        return "";
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "piTrove-18.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        g_logger.warn("NEWS: HTTP request to {} failed: {}", url, curl_easy_strerror(res));
        m_last_error.store(534);
        trigger_error(534);
        curl_easy_cleanup(curl);
        return "";
    }
    curl_easy_cleanup(curl);
    return response;
}

static std::string decode_html_entities(std::string text) {
    // Strip CDATA
    size_t cdata_pos;
    while ((cdata_pos = text.find("<![CDATA[")) != std::string::npos) {
        size_t end_pos = text.find("]]>", cdata_pos);
        if (end_pos != std::string::npos) {
            std::string inside = text.substr(cdata_pos + 9, end_pos - (cdata_pos + 9));
            text.replace(cdata_pos, (end_pos + 3) - cdata_pos, inside);
        } else {
            text.erase(cdata_pos, 9);
        }
    }

    auto replace_all = [](std::string& str, const std::string& from, const std::string& to) {
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    };

    replace_all(text, "&amp;", "&");
    replace_all(text, "&quot;", "\"");
    replace_all(text, "&#39;", "'");
    replace_all(text, "&apos;", "'");
    replace_all(text, "&lt;", "<");
    replace_all(text, "&gt;", ">");
    replace_all(text, "&nbsp;", " ");
    replace_all(text, "&#8217;", "'");
    replace_all(text, "&#8216;", "'");
    replace_all(text, "&#8220;", "\"");
    replace_all(text, "&#8221;", "\"");
    replace_all(text, "&#8211;", "-");
    replace_all(text, "&#8212;", "-");
    return trim(text);
}

static time_t parse_rfc822_date(const std::string& date_str) {
    if (date_str.empty()) return time(nullptr);
    struct tm tm_buf;
    std::memset(&tm_buf, 0, sizeof(tm_buf));
    const char* formats[] = {
        "%a, %d %b %Y %H:%M:%S %Z",
        "%a, %d %b %Y %H:%M:%S",
        "%d %b %Y %H:%M:%S %Z",
        "%d %b %Y %H:%M:%S",
        "%Y-%m-%dT%H:%M:%SZ",
        "%Y-%m-%d %H:%M:%S"
    };
    for (const char* fmt : formats) {
        if (strptime(date_str.c_str(), fmt, &tm_buf) != nullptr) {
            return timegm(&tm_buf);
        }
    }
    return time(nullptr);
}

void NewsTicker::parse_rss(const std::string& xml_data) {
    if (xml_data.empty()) return;

    std::regex item_regex("<item[ >]([\\s\\S]*?)</item>");
    std::regex title_regex("<title[^>]*>([\\s\\S]*?)</title>");
    std::regex pubdate_regex("<pubDate[^>]*>([\\s\\S]*?)</pubDate>");
    std::regex source_regex("<source[^>]*>([\\s\\S]*?)</source>");

    std::vector<NewsItem> parsed_items;
    auto items_begin = std::sregex_iterator(xml_data.begin(), xml_data.end(), item_regex);
    auto items_end = std::sregex_iterator();

    std::string tz = "UTC";
    {
        std::shared_lock lk(g_config_mtx);
        tz = g_cfg.timezone;
    }

    time_t now = time(nullptr);
    for (std::sregex_iterator i = items_begin; i != items_end; ++i) {
        std::string item_xml = (*i)[1].str();
        std::smatch m;
        std::string raw_title, raw_pubdate, raw_source;

        if (std::regex_search(item_xml, m, title_regex)) raw_title = m[1].str();
        if (std::regex_search(item_xml, m, pubdate_regex)) raw_pubdate = m[1].str();
        if (std::regex_search(item_xml, m, source_regex)) raw_source = m[1].str();

        std::string title = decode_html_entities(raw_title);
        if (title.empty()) continue;

        std::string source = decode_html_entities(raw_source);
        time_t pub_time = parse_rfc822_date(raw_pubdate);

        // Remove source suffix if already embedded in title (e.g. "Headline - CNN")
        size_t dash_pos = title.rfind(" - ");
        if (dash_pos != std::string::npos && dash_pos > title.length() / 2) {
            if (source.empty()) source = title.substr(dash_pos + 3);
            title = title.substr(0, dash_pos);
        }

        NewsItem item;
        item.title = title;
        item.source = source;
        item.published_time = pub_time;
        item.formatted_time = format_epoch_tz(pub_time, tz, "%I:%M %p");

        parsed_items.push_back(std::move(item));
        if (parsed_items.size() >= 30) break;
    }

    if (!parsed_items.empty()) {
        std::unique_lock lk(m_items_mtx);
        m_items = std::move(parsed_items);
        m_cached_segments.clear();
        m_cached_total_width = 0;
        m_last_error.store(0);
        m_last_fetch_time.store(now);
        g_logger.info("NEWS: Successfully loaded {} headlines in timezone {}", m_items.size(), tz);
    } else {
        g_logger.warn("NEWS: No valid items parsed from feed");
    }
}

void NewsTicker::fetch_sync() {
    if (m_fetching.exchange(true)) return;
    struct Guard { std::atomic<bool>& f; ~Guard() { f.store(false); } } g{m_fetching};

    std::string source = "global";
    std::string local_query = "US";
    {
        std::shared_lock lk(g_config_mtx);
        source = g_cfg.news_source;
        local_query = g_cfg.news_local_query;
    }

    std::string url;
    if (source == "local") {
        std::string q = local_query.empty() ? "US" : local_query;
        url = std::format("https://news.google.com/rss/headlines/section/topic/NATION?hl=en-{}&gl={}&ceid={}:en", q, q, q);
    } else {
        url = "https://news.google.com/rss?hl=en-US&gl=US&ceid=US:en";
    }

    g_logger.info("NEWS: Fetching live headlines from {}", url);
    std::string xml = execute_http_get(url);
    if (xml.empty()) {
        // Secondary fallback to BBC World News RSS
        url = "http://feeds.bbci.co.uk/news/world/rss.xml";
        g_logger.info("NEWS: Attempting fallback feed: {}", url);
        xml = execute_http_get(url);
    }

    if (!xml.empty()) {
        parse_rss(xml);
    }
}

bool NewsTicker::start() {
    stop();
    m_running.store(true);
    m_scroll_offset = 0.0f;
    m_last_render_ticks = SDL_GetTicks();

    if (!spawn_thread_safe(m_worker_thread, "news_ticker", [this]() {
        // Initial fetch
        this->fetch_sync();

        while (this->m_running.load()) {
            int refresh_mins = 15;
            {
                std::shared_lock lk(g_config_mtx);
                refresh_mins = std::clamp(g_cfg.news_refresh_minutes, 5, 120);
            }
            // Sleep in 1s increments for fast responsiveness to stop()
            for (int i = 0; i < refresh_mins * 60 && this->m_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (this->m_running.load()) {
                this->fetch_sync();
            }
        }
    })) {
        g_logger.error("NEWS: Failed to spawn background worker thread");
        m_running.store(false);
        return false;
    }
    return true;
}

void NewsTicker::stop() {
    m_running.store(false);
}

std::vector<NewsItem> NewsTicker::get_items() const {
    std::shared_lock lk(m_items_mtx);
    return m_items;
}

std::string NewsTicker::get_status_json() const {
    std::shared_lock lk(m_items_mtx);
    std::ostringstream ss;
    ss << "{\"count\":" << m_items.size()
       << ",\"last_fetch\":" << m_last_fetch_time.load()
       << ",\"last_error\":" << m_last_error.load()
       << ",\"items\":[";
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"title\":\"" << escape_shell_arg(m_items[i].title) << "\""
           << ",\"source\":\"" << escape_shell_arg(m_items[i].source) << "\""
           << ",\"time\":\"" << m_items[i].formatted_time << "\"}";
    }
    ss << "]}";
    return ss.str();
}

void NewsTicker::render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w) {
    if (!renderer || !font_renderer || bounds.h <= 0 || bounds.w <= 0) return;

    // 1. Draw glassmorphic background
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 10, 14, 22, 235); // Sleek obsidian/slate backdrop
    SDL_FRect bg_rect = { (float)bounds.x, (float)bounds.y, (float)bounds.w, (float)bounds.h };
    SDL_RenderFillRect(renderer, &bg_rect);

    // Accent top border line (vibrant cyan / electric blue)
    SDL_SetRenderDrawColor(renderer, 0, 200, 255, 180);
    SDL_FRect top_line = { (float)bounds.x, (float)bounds.y, (float)bounds.w, 2.0f };
    SDL_RenderFillRect(renderer, &top_line);

    int font_size = 14;
    int scroll_speed = 60;
    {
        std::shared_lock lk_cfg(g_config_mtx);
        font_size = std::clamp(g_cfg.news_font_size, 10, 24);
        scroll_speed = std::clamp(g_cfg.news_scroll_speed, 10, 300);
    }
    font_size = (int)round((double)font_size * screen_w / 1920.0);

    FontHandle& font = font_renderer->load_font(font_path, font_size);

    // 2. Build individual segments if empty or font size changed
    {
        std::shared_lock lk(m_items_mtx);
        if (m_cached_segments.empty() || m_cached_font_size != font_size) {
            m_cached_segments.clear();
            m_cached_total_width = 0;
            m_cached_font_size = font_size;

            if (m_items.empty()) {
                std::string default_str = "   piTrove Live News  •  Updating latest headlines...   •   ";
                int w = 0, h = 0;
                font_renderer->measure(font, default_str, w, h);
                m_cached_segments.push_back({default_str, w});
                m_cached_total_width = w;
            } else {
                for (const auto& item : m_items) {
                    std::string seg = "   [" + item.formatted_time + "] " + item.title;
                    if (!item.source.empty()) seg += " (" + item.source + ")";
                    seg += "   •   ";
                    int w = 0, h = 0;
                    font_renderer->measure(font, seg, w, h);
                    m_cached_segments.push_back({seg, w});
                    m_cached_total_width += w;
                }
            }
            if (m_cached_total_width <= 0) m_cached_total_width = 100;
        }
    }

    // 3. Compute continuous scrolling offset
    uint64_t now_ticks = SDL_GetTicks();
    if (m_last_render_ticks == 0) m_last_render_ticks = now_ticks;
    float dt = (now_ticks - m_last_render_ticks) / 1000.0f;
    m_last_render_ticks = now_ticks;
    if (dt > 0.5f) dt = 0.016f; // Avoid jump on pause/unpause

    m_scroll_offset += (float)scroll_speed * dt;
    if (m_scroll_offset >= (float)m_cached_total_width) {
        m_scroll_offset = std::fmod(m_scroll_offset, (float)m_cached_total_width);
    }

    // Fixed left pinned badge: [ LIVE NEWS ]
    int badge_w = (int)round(150.0 * screen_w / 1920.0);
    int visible_start_x = bounds.x + badge_w;
    int visible_end_x = bounds.x + bounds.w;

    // Set clipping rectangle so text only scrolls in the visible stream area
    SDL_Rect clip_rect = { visible_start_x, bounds.y, bounds.w - badge_w, bounds.h };
    SDL_SetRenderClipRect(renderer, &clip_rect);

    int text_y = bounds.y + (bounds.h - font_size) / 2 - 2;
    int cur_x = visible_start_x - (int)m_scroll_offset;

    // Fast forward to first visible segment
    size_t start_seg_idx = 0;
    while (start_seg_idx < m_cached_segments.size() && cur_x + m_cached_segments[start_seg_idx].second < visible_start_x) {
        cur_x += m_cached_segments[start_seg_idx].second;
        start_seg_idx++;
    }

    // Draw all segments visible in the viewport, wrapping around smoothly
    size_t seg_idx = start_seg_idx;
    int loop_guard = 0;
    while (cur_x < visible_end_x && !m_cached_segments.empty() && ++loop_guard < 100) {
        const auto& seg = m_cached_segments[seg_idx % m_cached_segments.size()];
        if (cur_x + seg.second > visible_start_x && cur_x < visible_end_x) {
            font_renderer->draw_text(cur_x, text_y, font, seg.first, 235, 240, 250, 255);
        }
        cur_x += seg.second;
        seg_idx++;
    }

    // Reset clip rect before drawing badge
    SDL_SetRenderClipRect(renderer, nullptr);

    // 4. Render pinned left badge with pulsing indicator
    SDL_FRect badge_rect = { (float)bounds.x, (float)bounds.y, (float)badge_w, (float)bounds.h };
    SDL_SetRenderDrawColor(renderer, 18, 24, 38, 250);
    SDL_RenderFillRect(renderer, &badge_rect);

    // Subtle divider line
    SDL_SetRenderDrawColor(renderer, 0, 200, 255, 120);
    SDL_FRect divider_rect = { (float)(bounds.x + badge_w - 2), (float)bounds.y, 2.0f, (float)bounds.h };
    SDL_RenderFillRect(renderer, &divider_rect);

    // Red live pulsing dot
    int dot_r = (int)round(4.0 * screen_w / 1920.0);
    int dot_x = bounds.x + (int)round(16.0 * screen_w / 1920.0);
    int dot_y = bounds.y + bounds.h / 2;
    SDL_FRect dot_rect = { (float)(dot_x - dot_r), (float)(dot_y - dot_r), (float)(dot_r * 2), (float)(dot_r * 2) };
    SDL_SetRenderDrawColor(renderer, 255, 60, 60, 255);
    SDL_RenderFillRect(renderer, &dot_rect);

    // Badge text
    std::string badge_label = "LIVE NEWS";
    font_renderer->draw_text(dot_x + dot_r + 8, text_y, font, badge_label, 255, 255, 255, 255);
}
