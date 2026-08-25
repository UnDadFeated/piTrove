#include "news_ticker.h"
#include "config.h"
#include "util.h"
#include "error_db.h"
#include <curl/curl.h>
#include <regex>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <algorithm>

static size_t news_curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* mem = static_cast<std::string*>(userp);
    mem->append(static_cast<char*>(contents), total_size);
    return total_size;
}

static std::string decode_html_entities(const std::string& input) {
    std::string out = input;
    std::vector<std::pair<std::string, std::string>> entities = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
        {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}, {"&mdash;", "—"},
        {"&ndash;", "–"}, {"&hellip;", "..."}
    };
    for (const auto& [entity, rep] : entities) {
        size_t pos = 0;
        while ((pos = out.find(entity, pos)) != std::string::npos) {
            out.replace(pos, entity.length(), rep);
            pos += rep.length();
        }
    }
    // Strip any lingering HTML tags
    std::regex tag_regex("<[^>]*>");
    out = std::regex_replace(out, tag_regex, "");
    return trim(out);
}

static std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else if (c == ' ') {
            escaped << '+';
        } else {
            escaped << '%' << std::setw(2) << ((int)(unsigned char)c);
        }
    }
    return escaped.str();
}

static time_t parse_rfc822_date(const std::string& date_str) {
    if (date_str.empty()) return time(nullptr);
    struct tm tm_buf {};
    const char* formats[] = {
        "%a, %d %b %Y %H:%M:%S %Z",
        "%a, %d %b %Y %H:%M:%S",
        "%d %b %Y %H:%M:%S %Z",
        "%d %b %Y %H:%M:%S",
        "%Y-%m-%dT%H:%M:%SZ"
    };
    for (const char* fmt : formats) {
        if (strptime(date_str.c_str(), fmt, &tm_buf) != nullptr) {
            return timegm(&tm_buf);
        }
    }
    return time(nullptr);
}

NewsTicker::NewsTicker() = default;

NewsTicker::~NewsTicker() {
    stop();
}

std::string NewsTicker::execute_http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        m_last_error.store(537); // E537: NEWS_NETWORK_TIMEOUT
        return "";
    }

    std::string response_string;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, news_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko)");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        g_logger.warn("NEWS: HTTP GET failed for {}: curl_res={}, http_code={}", url, (int)res, http_code);
        m_last_error.store(537);
        return "";
    }

    return response_string;
}

std::vector<NewsItem> NewsTicker::parse_rss(const std::string& xml_data) {
    std::vector<NewsItem> parsed_items;
    if (xml_data.empty()) return parsed_items;

    std::regex item_regex("<item[ >]([\\s\\S]*?)</item>");
    std::regex title_regex("<title[^>]*>([\\s\\S]*?)</title>");
    std::regex pubdate_regex("<pubDate[^>]*>([\\s\\S]*?)</pubDate>");
    std::regex source_regex("<source[^>]*>([\\s\\S]*?)</source>");
    std::regex link_regex("<link[^>]*>([\\s\\S]*?)</link>");
    std::regex guid_regex("<guid[^>]*>([\\s\\S]*?)</guid>");

    auto items_begin = std::sregex_iterator(xml_data.begin(), xml_data.end(), item_regex);
    auto items_end = std::sregex_iterator();

    std::string tz = "UTC";
    std::vector<std::string> blacklist;
    {
        std::shared_lock lk(g_config_mtx);
        tz = g_cfg.timezone;
        blacklist = g_cfg.news_blacklist;
    }

    auto is_blacklisted = [&](const std::string& text) {
        if (text.empty() || blacklist.empty()) return false;
        std::string lower_text = text;
        std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
        for (const auto& b : blacklist) {
            if (b.empty()) continue;
            std::string lower_b = b;
            std::transform(lower_b.begin(), lower_b.end(), lower_b.begin(), ::tolower);
            if (lower_text.find(lower_b) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    for (std::sregex_iterator i = items_begin; i != items_end; ++i) {
        std::string item_xml = (*i)[1].str();
        std::smatch m;
        std::string raw_title, raw_pubdate, raw_source;
        std::string raw_link, raw_guid;

        if (std::regex_search(item_xml, m, title_regex)) raw_title = m[1].str();
        if (std::regex_search(item_xml, m, pubdate_regex)) raw_pubdate = m[1].str();
        if (std::regex_search(item_xml, m, source_regex)) raw_source = m[1].str();
        if (std::regex_search(item_xml, m, link_regex)) raw_link = m[1].str();
        if (std::regex_search(item_xml, m, guid_regex)) raw_guid = m[1].str();

        std::string title = decode_html_entities(raw_title);
        if (title.empty()) continue;

        std::string source = decode_html_entities(raw_source);
        std::string link = decode_html_entities(raw_link);
        std::string guid = decode_html_entities(raw_guid);

        // Remove source suffix if already embedded in title (e.g. "Headline - CNN")
        size_t dash_pos = title.rfind(" - ");
        if (dash_pos != std::string::npos && dash_pos > title.length() / 2) {
            if (source.empty()) source = title.substr(dash_pos + 3);
            title = title.substr(0, dash_pos);
        }

        // Apply Blacklist filter (checks source network, title, link, and guid)
        if (is_blacklisted(source) || is_blacklisted(title) || is_blacklisted(link) || is_blacklisted(guid)) {
            g_logger.info("NEWS: Filtered out blacklisted headline '{}' (source: '{}')", title, source);
            continue;
        }

        time_t pub_time = parse_rfc822_date(raw_pubdate);

        NewsItem item;
        item.title = title;
        item.source = source;
        item.published_time = pub_time;
        item.formatted_time = format_epoch_tz(pub_time, tz, "%I:%M %p");

        parsed_items.push_back(std::move(item));
        if (parsed_items.size() >= 30) break;
    }

    return parsed_items;
}

void NewsTicker::fetch_sync() {
    if (m_fetching.exchange(true)) return;
    struct Guard { std::atomic<bool>& f; ~Guard() { f.store(false); } } g{m_fetching};

    std::string local_q = "US";
    std::string tz = "UTC";
    {
        std::shared_lock lk(g_config_mtx);
        if (!g_cfg.news_local_query.empty()) {
            local_q = g_cfg.news_local_query;
        }
        tz = g_cfg.timezone;
    }

    time_t now = time(nullptr);

    // 1. Fetch Local News (by zipcode / local area for past 1 week)
    std::string local_query_with_time = local_q;
    if (local_query_with_time.find("when:") == std::string::npos) {
        local_query_with_time += " when:7d";
    }
    std::string local_url = "https://news.google.com/rss/search?q=" + url_encode(local_query_with_time) + "&hl=en-US&gl=US&ceid=US:en";
    g_logger.info("NEWS: Fetching local headlines (past 1 week) from {}", local_url);
    std::string local_xml = execute_http_get(local_url);
    
    std::vector<NewsItem> raw_local = parse_rss(local_xml);
    
    // If zipcode alone yielded fewer than 3 headlines, try combining with nearby city/community query
    if (raw_local.size() < 3 && local_q == "95624") {
        std::string broader_url = "https://news.google.com/rss/search?q=" + url_encode("95624 OR \"Elk Grove\" when:7d") + "&hl=en-US&gl=US&ceid=US:en";
        g_logger.info("NEWS: Enriching local query with community search: {}", broader_url);
        std::string broader_xml = execute_http_get(broader_url);
        if (!broader_xml.empty()) {
            raw_local = parse_rss(broader_xml);
        }
    }

    std::vector<NewsItem> local_items;
    time_t one_week_ago = now - (7 * 86400);
    for (auto& item : raw_local) {
        if (item.published_time == 0 || item.published_time >= one_week_ago) {
            local_items.push_back(std::move(item));
        }
    }
    if (local_items.empty()) {
        local_items = std::move(raw_local);
    }

    // Sort local headlines descending by timestamp (newest first)
    std::sort(local_items.begin(), local_items.end(), [](const NewsItem& a, const NewsItem& b) {
        return a.published_time > b.published_time;
    });

    // 2. Fetch World News (Google Top Stories for past 6 hours)
    std::string global_url = "https://news.google.com/rss?hl=en-US&gl=US&ceid=US:en";
    g_logger.info("NEWS: Fetching Google top stories (past 6 hours) from {}", global_url);
    std::string global_xml = execute_http_get(global_url);
    std::vector<NewsItem> raw_global = parse_rss(global_xml);
    
    std::vector<NewsItem> global_items;
    time_t six_hours_ago = now - (6 * 3600);
    for (auto& item : raw_global) {
        if (item.published_time >= six_hours_ago) {
            global_items.push_back(std::move(item));
        }
    }
    // If fewer than 5 top stories in the exact 6h window, include freshest items from Google top stories
    if (global_items.size() < 5) {
        for (auto& item : raw_global) {
            bool exists = false;
            for (const auto& gi : global_items) {
                if (gi.title == item.title) { exists = true; break; }
            }
            if (!exists) global_items.push_back(std::move(item));
            if (global_items.size() >= 25) break;
        }
    }

    // Sort world headlines descending by timestamp (newest first)
    std::sort(global_items.begin(), global_items.end(), [](const NewsItem& a, const NewsItem& b) {
        return a.published_time > b.published_time;
    });

    now = time(nullptr);
    {
        std::unique_lock lk(m_items_mtx);
        m_local_items = std::move(local_items);
        m_global_items = std::move(global_items);
        m_cached_local_spans.clear();
        m_cached_global_spans.clear();
        m_cached_local_total_w = 0;
        m_cached_global_total_w = 0;
        m_last_error.store(0);
        m_last_fetch_time.store(now);
        g_logger.info("NEWS: Successfully loaded {} local ({}) and {} global headlines in timezone {}",
                      m_local_items.size(), local_q, m_global_items.size(), tz);
    }
}

void NewsTicker::sync() {
    fetch_sync();
}

bool NewsTicker::start() {
    stop();
    m_running.store(true);
    m_local_scroll_offset = 0.0f;
    m_global_scroll_offset = 0.0f;
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
    std::vector<NewsItem> all = m_local_items;
    all.insert(all.end(), m_global_items.begin(), m_global_items.end());
    return all;
}

std::string NewsTicker::get_status_json() const {
    std::shared_lock lk(m_items_mtx);
    std::ostringstream ss;
    ss << "{\"local_count\":" << m_local_items.size()
       << ",\"global_count\":" << m_global_items.size()
       << ",\"last_sync\":" << m_last_fetch_time.load()
       << ",\"last_error\":" << m_last_error.load()
       << ",\"local_items\":[";
    for (size_t i = 0; i < m_local_items.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"title\":\"" << escape_shell_arg(m_local_items[i].title) << "\""
           << ",\"source\":\"" << escape_shell_arg(m_local_items[i].source) << "\""
           << ",\"time\":\"" << m_local_items[i].formatted_time << "\"}";
    }
    ss << "],\"global_items\":[";
    for (size_t i = 0; i < m_global_items.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"title\":\"" << escape_shell_arg(m_global_items[i].title) << "\""
           << ",\"source\":\"" << escape_shell_arg(m_global_items[i].source) << "\""
           << ",\"time\":\"" << m_global_items[i].formatted_time << "\"}";
    }
    ss << "]}";
    return ss.str();
}

void NewsTicker::render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w) {
    if (!renderer || !font_renderer || bounds.h <= 0 || bounds.w <= 0) return;

    int font_size = 11;
    int scroll_speed = 32;
    std::string local_badge_name = "LOCAL";
    {
        std::shared_lock lk_cfg(g_config_mtx);
        font_size = std::clamp((int)round((double)g_cfg.news_font_size * screen_w / 1920.0), 9, 16);
        scroll_speed = std::clamp(g_cfg.news_scroll_speed, 10, 150);
        if (!g_cfg.news_local_query.empty()) {
            local_badge_name = "LOCAL " + g_cfg.news_local_query;
        }
    }

    FontHandle& font = font_renderer->load_font(font_path, font_size);

    // 1. Sleek Obsidian Glass backdrop
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 10, 14, 22, 245);
    SDL_FRect bg_rect = { (float)bounds.x, (float)bounds.y, (float)bounds.w, (float)bounds.h };
    SDL_RenderFillRect(renderer, &bg_rect);

    // Skinny 2-line layout
    int row_h = bounds.h / 2;

    // 2. Prepare tokens and measure total width for both rows
    {
        std::shared_lock lk(m_items_mtx);

        if (m_cached_local_spans.empty() || m_cached_font_size != font_size) {
            m_cached_local_spans.clear();
            m_cached_local_total_w = 0;
            m_cached_font_size = font_size;

            if (m_local_items.empty()) {
                NewsHeadlineSpan span;
                std::string s = "   piTrove Local News  •  Updating latest headlines...   •   ";
                int w = 0, h = 0;
                font_renderer->measure(font, s, w, h);
                span.tokens.push_back({s, w, {200, 215, 230, 255}});
                span.total_w = w;
                m_cached_local_spans.push_back(span);
                m_cached_local_total_w = w;
            } else {
                for (const auto& item : m_local_items) {
                    NewsHeadlineSpan span;
                    int item_w = 0;

                    // Timestamp in Warm Amber / Gold
                    if (!item.formatted_time.empty()) {
                        std::string t_str = "   [" + item.formatted_time + "] ";
                        int tw = 0, th = 0;
                        font_renderer->measure(font, t_str, tw, th);
                        span.tokens.push_back({t_str, tw, {255, 195, 60, 255}});
                        item_w += tw;
                    } else {
                        std::string sp = "   ";
                        int tw = 0, th = 0;
                        font_renderer->measure(font, sp, tw, th);
                        span.tokens.push_back({sp, tw, {255, 255, 255, 255}});
                        item_w += tw;
                    }

                    // Headline Title in Crisp White
                    int title_w = 0, title_h = 0;
                    font_renderer->measure(font, item.title, title_w, title_h);
                    span.tokens.push_back({item.title, title_w, {245, 248, 255, 255}});
                    item_w += title_w;

                    // Source in Sky Cyan
                    if (!item.source.empty()) {
                        std::string src_str = " (" + item.source + ")";
                        int sw = 0, sh = 0;
                        font_renderer->measure(font, src_str, sw, sh);
                        span.tokens.push_back({src_str, sw, {0, 200, 255, 230}});
                        item_w += sw;
                    }

                    // Separator bullet in Electric Cyan
                    std::string bul = "   •   ";
                    int bw = 0, bh = 0;
                    font_renderer->measure(font, bul, bw, bh);
                    span.tokens.push_back({bul, bw, {255, 180, 50, 200}});
                    item_w += bw;

                    span.total_w = item_w;
                    m_cached_local_spans.push_back(span);
                    m_cached_local_total_w += item_w;
                }
            }
            if (m_cached_local_total_w <= 0) m_cached_local_total_w = 100;
        }

        if (m_cached_global_spans.empty() || m_cached_font_size != font_size) {
            m_cached_global_spans.clear();
            m_cached_global_total_w = 0;

            if (m_global_items.empty()) {
                NewsHeadlineSpan span;
                std::string s = "   piTrove World News  •  Updating latest headlines...   •   ";
                int w = 0, h = 0;
                font_renderer->measure(font, s, w, h);
                span.tokens.push_back({s, w, {200, 215, 230, 255}});
                span.total_w = w;
                m_cached_global_spans.push_back(span);
                m_cached_global_total_w = w;
            } else {
                for (const auto& item : m_global_items) {
                    NewsHeadlineSpan span;
                    int item_w = 0;

                    // Timestamp in Sky Ice Blue
                    if (!item.formatted_time.empty()) {
                        std::string t_str = "   [" + item.formatted_time + "] ";
                        int tw = 0, th = 0;
                        font_renderer->measure(font, t_str, tw, th);
                        span.tokens.push_back({t_str, tw, {100, 215, 255, 255}});
                        item_w += tw;
                    } else {
                        std::string sp = "   ";
                        int tw = 0, th = 0;
                        font_renderer->measure(font, sp, tw, th);
                        span.tokens.push_back({sp, tw, {255, 255, 255, 255}});
                        item_w += tw;
                    }

                    // Headline Title in Crisp White
                    int title_w = 0, title_h = 0;
                    font_renderer->measure(font, item.title, title_w, title_h);
                    span.tokens.push_back({item.title, title_w, {245, 248, 255, 255}});
                    item_w += title_w;

                    // Source in Soft Mint
                    if (!item.source.empty()) {
                        std::string src_str = " (" + item.source + ")";
                        int sw = 0, sh = 0;
                        font_renderer->measure(font, src_str, sw, sh);
                        span.tokens.push_back({src_str, sw, {120, 225, 180, 230}});
                        item_w += sw;
                    }

                    // Separator bullet in Electric Cyan
                    std::string bul = "   •   ";
                    int bw = 0, bh = 0;
                    font_renderer->measure(font, bul, bw, bh);
                    span.tokens.push_back({bul, bw, {0, 210, 255, 200}});
                    item_w += bw;

                    span.total_w = item_w;
                    m_cached_global_spans.push_back(span);
                    m_cached_global_total_w += item_w;
                }
            }
            if (m_cached_global_total_w <= 0) m_cached_global_total_w = 100;
        }
    }

    // 3. Compute continuous scrolling offset for each row
    uint64_t now_ticks = SDL_GetTicks();
    if (m_last_render_ticks == 0) m_last_render_ticks = now_ticks;
    float dt = (now_ticks - m_last_render_ticks) / 1000.0f;
    m_last_render_ticks = now_ticks;
    if (dt > 0.5f) dt = 0.016f;

    m_local_scroll_offset += (float)scroll_speed * dt;
    if (m_local_scroll_offset >= (float)m_cached_local_total_w) {
        m_local_scroll_offset = std::fmod(m_local_scroll_offset, (float)m_cached_local_total_w);
    }

    m_global_scroll_offset += (float)(scroll_speed * 1.05f) * dt;
    if (m_global_scroll_offset >= (float)m_cached_global_total_w) {
        m_global_scroll_offset = std::fmod(m_global_scroll_offset, (float)m_cached_global_total_w);
    }

    // Fixed left pinned badge (with padding inside visible matte margin)
    int badge_w = (int)round(110.0 * screen_w / 1920.0);
    bool has_matting = false;
    int mat_size = 0;
    {
        std::shared_lock lk_cfg(g_config_mtx);
        has_matting = g_cfg.matting;
        mat_size = (int)round((double)g_cfg.matting_size * screen_w / 1920.0);
    }
    int badge_start_x = bounds.x + (has_matting ? (int)round(((double)mat_size * 0.5) + 10.0) : (int)round(16.0 * screen_w / 1920.0));
    int visible_start_x = badge_start_x + badge_w + (int)round(6.0 * screen_w / 1920.0);
    int visible_end_x = has_matting ? (screen_w - (int)round((double)mat_size * 0.8)) : (bounds.x + bounds.w);

    // Helper lambda to render colorized scrolling news row
    auto render_news_row = [&](int y_offset, float scroll_offset, const std::vector<NewsHeadlineSpan>& spans) {
        SDL_Rect clip_rect = { visible_start_x, bounds.y + y_offset, bounds.w - badge_w, row_h };
        SDL_SetRenderClipRect(renderer, &clip_rect);

        int text_y = bounds.y + y_offset + (row_h - font_size) / 2 - 1;
        int cur_x = visible_start_x - (int)scroll_offset;

        size_t start_span_idx = 0;
        while (start_span_idx < spans.size() && cur_x + spans[start_span_idx].total_w < visible_start_x) {
            cur_x += spans[start_span_idx].total_w;
            start_span_idx++;
        }

        size_t span_idx = start_span_idx;
        int loop_guard = 0;
        while (cur_x < visible_end_x && !spans.empty() && ++loop_guard < 100) {
            const auto& span = spans[span_idx % spans.size()];
            if (cur_x + span.total_w > visible_start_x && cur_x < visible_end_x) {
                int tok_x = cur_x;
                for (const auto& tok : span.tokens) {
                    if (tok_x + tok.w > visible_start_x && tok_x < visible_end_x) {
                        font_renderer->draw_text(tok_x, text_y, font, tok.text, tok.color.r, tok.color.g, tok.color.b, tok.color.a);
                    }
                    tok_x += tok.w;
                }
            }
            cur_x += span.total_w;
            span_idx++;
        }

        SDL_SetRenderClipRect(renderer, nullptr);
    };

    // Render Row 1: Local News
    render_news_row(0, m_local_scroll_offset, m_cached_local_spans);

    // Render Row 2: Global News
    render_news_row(row_h, m_global_scroll_offset, m_cached_global_spans);

    // 4. Render pinned left badges
    auto render_badge = [&](int y_offset, const std::string& label, uint8_t dot_r_c, uint8_t dot_g_c, uint8_t dot_b_c) {
        SDL_FRect b_rect = { 0.0f, (float)(bounds.y + y_offset), (float)(badge_start_x + badge_w), (float)row_h };
        SDL_SetRenderDrawColor(renderer, 18, 24, 38, 250);
        SDL_RenderFillRect(renderer, &b_rect);

        SDL_SetRenderDrawColor(renderer, 0, 200, 255, 100);
        SDL_FRect div = { (float)(badge_start_x + badge_w - 2), (float)(bounds.y + y_offset), 2.0f, (float)row_h };
        SDL_RenderFillRect(renderer, &div);

        int dot_r = (int)round(3.0 * screen_w / 1920.0);
        int dot_x = badge_start_x + (int)round(10.0 * screen_w / 1920.0);
        int dot_y = bounds.y + y_offset + row_h / 2;
        SDL_FRect dot = { (float)(dot_x - dot_r), (float)(dot_y - dot_r), (float)(dot_r * 2), (float)(dot_r * 2) };
        SDL_SetRenderDrawColor(renderer, dot_r_c, dot_g_c, dot_b_c, 255);
        SDL_RenderFillRect(renderer, &dot);

        int text_y = bounds.y + y_offset + (row_h - font_size) / 2 - 1;
        font_renderer->draw_text(dot_x + dot_r + 6, text_y, font, label, 255, 255, 255, 255);
    };

    // Badge 1: LOCAL (Slate Grey dot)
    render_badge(0, local_badge_name, 140, 150, 165);

    // Badge 2: WORLD (Slate Grey dot)
    render_badge(row_h, "WORLD", 140, 150, 165);

    // Accent top border line (vibrant cyan / electric blue) - drawn last across entire screen
    SDL_SetRenderDrawColor(renderer, 0, 200, 255, 220);
    SDL_FRect top_line = { (float)bounds.x, (float)bounds.y, (float)bounds.w, 2.0f };
    SDL_RenderFillRect(renderer, &top_line);
}
