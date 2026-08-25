#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <shared_mutex>
#include <SDL3/SDL.h>
#include "font_render.h"

inline std::vector<std::string> get_stock_preset_symbols(const std::string& preset) {
    if (preset == "big_tech") {
        return {"AAPL", "MSFT", "NVDA", "GOOGL", "AMZN", "META", "TSLA"};
    } else if (preset == "semiconductors") {
        return {"NVDA", "AVGO", "TSM", "AMD", "QCOM", "INTC", "ASML", "MU", "AMAT", "TXN"};
    } else if (preset == "dividend_kings") {
        return {"JNJ", "PG", "KO", "PEP", "ABBV", "MMM", "CL", "TGT", "CVX", "XOM"};
    }
    // Default: sp500_top10
    return {"NVDA", "AAPL", "MSFT", "AMZN", "GOOGL", "META", "BRK-B", "TSLA", "AVGO", "JPM"};
}

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
