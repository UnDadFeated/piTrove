#ifndef PITROVE_NEWS_TICKER_H
#define PITROVE_NEWS_TICKER_H

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include "font_render.h"

struct NewsItem {
    std::string title;
    std::string source;
    time_t published_time{0};
    std::string formatted_time;
};

class NewsTicker {
private:
    std::vector<NewsItem> m_items;
    mutable std::shared_mutex m_items_mtx;
    std::jthread m_worker_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_fetching{false};
    std::atomic<int> m_last_error{0};
    std::atomic<time_t> m_last_fetch_time{0};

    float m_scroll_offset{0.0f};
    uint64_t m_last_render_ticks{0};
    std::string m_cached_ticker_str{};
    int m_cached_text_width{0};

    std::string execute_http_get(const std::string& url);
    void parse_rss(const std::string& xml_data);

public:
    NewsTicker();
    ~NewsTicker();

    bool start();
    void stop();

    void fetch_sync();
    std::vector<NewsItem> get_items() const;
    std::string get_status_json() const;
    int get_last_error() const { return m_last_error.load(); }

    void render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w);
};

inline NewsTicker g_news_ticker;

#endif // PITROVE_NEWS_TICKER_H
