#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <shared_mutex>
#include <SDL3/SDL.h>
#include "font_render.h"

struct StockQuote {
    std::string symbol;
    std::string display_symbol;
    std::string name;
    double price{0.0};
    double prev_close{0.0};
    double change{0.0};
    double change_pct{0.0};
    double aftermarket_price{0.0};
    double aftermarket_change_pct{0.0};
    bool has_aftermarket{false};
    bool is_crypto{false};
    std::string formatted_price;
    std::string formatted_aftermarket;
    uint64_t last_updated_epoch{0};
};

class StockStreamer {
public:
    StockStreamer() = default;
    ~StockStreamer() { stop(); }

    bool start();
    void stop();
    void sync();
    void fetch_sync();

    std::vector<StockQuote> get_stocks() const;
    StockQuote get_crypto() const;
    std::string get_status_json() const;

    void render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w);

private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_fetching{false};
    std::jthread m_worker_thread;

    mutable std::shared_mutex m_quotes_mtx;
    std::vector<StockQuote> m_stocks;
    StockQuote m_crypto;
    std::atomic<time_t> m_last_sync_time{0};
    std::atomic<int> m_last_error{0};
};

inline StockStreamer g_stock_streamer;
