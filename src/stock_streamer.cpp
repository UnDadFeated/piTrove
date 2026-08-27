#include "stock_streamer.h"
#include "config.h"
#include "util.h"
#include <curl/curl.h>
#include <algorithm>
#include <sstream>
#include <format>
#include <cmath>
#include <regex>
#include <chrono>
#include <future>

extern Config g_cfg;
extern std::shared_mutex g_config_mtx;
extern Logger g_logger;

static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* mem = static_cast<std::string*>(userp);
    mem->append(static_cast<char*>(contents), total_size);
    return total_size;
}

static std::string http_get_json(const std::string& url, int timeout_secs = 4) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_secs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return "";
    }
    return response;
}

static std::string format_currency(double val, bool is_crypto = false) {
    if (is_crypto) {
        int64_t whole = (int64_t)val;
        int cents = (int)std::round((val - (double)whole) * 100.0);
        if (cents < 0) cents = 0;
        if (cents >= 100) { whole++; cents -= 100; }
        std::string s = std::to_string(whole);
        std::string res = "";
        int count = 0;
        for (int i = (int)s.length() - 1; i >= 0; --i) {
            res += s[i];
            if (++count == 3 && i > 0) {
                res += ',';
                count = 0;
            }
        }
        std::reverse(res.begin(), res.end());
        return std::format("${}.{:02d}", res, cents);
    }
    return std::format("${:.2f}", val);
}

static std::string get_company_name(const std::string& sym) {
    if (sym == "NVDA") return "NVIDIA";
    if (sym == "AAPL") return "Apple";
    if (sym == "MSFT") return "Microsoft";
    if (sym == "AMZN") return "Amazon";
    if (sym == "GOOGL" || sym == "GOOG") return "Alphabet";
    if (sym == "META") return "Meta";
    if (sym == "BRK-B" || sym == "BRK.B") return "Berkshire";
    if (sym == "TSLA") return "Tesla";
    if (sym == "AVGO") return "Broadcom";
    if (sym == "JPM") return "JPMorgan";
    if (sym == "BTC-USD" || sym == "BTC") return "Bitcoin";
    return sym;
}

static std::string clean_display_sym(const std::string& sym) {
    if (sym == "BRK-B") return "BRK.B";
    if (sym == "BTC-USD") return "BTC";
    return sym;
}

static bool parse_yahoo_chart_meta(const std::string& json_str, StockQuote& quote) {
    if (json_str.empty()) return false;

    auto extract_double = [&](const std::string& key, double default_val = 0.0) -> double {
        std::regex re("\"" + key + "\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)");
        std::smatch m;
        if (std::regex_search(json_str, m, re)) {
            try { return std::stod(m[1].str()); } catch (...) {}
        }
        return default_val;
    };

    double reg_price = extract_double("regularMarketPrice", 0.0);
    double prev_close = extract_double("chartPreviousClose", 0.0);
    if (prev_close <= 0.0) prev_close = extract_double("previousClose", 0.0);

    if (reg_price <= 0.0) return false;

    quote.price = reg_price;
    quote.prev_close = prev_close;
    quote.change = (prev_close > 0.0) ? (reg_price - prev_close) : 0.0;
    quote.change_pct = (prev_close > 0.0) ? ((reg_price - prev_close) / prev_close * 100.0) : 0.0;

    double post_price = extract_double("postMarketPrice", 0.0);
    double pre_price = extract_double("preMarketPrice", 0.0);
    double ah_price = (post_price > 0.0) ? post_price : ((pre_price > 0.0) ? pre_price : 0.0);

    // Traditional stocks: fallback to latest extended-hours candle from chart 1m series (Crypto is 24/7 continuous spot)
    if (!quote.is_crypto && ah_price <= 0.0) {
        int64_t reg_end = 0;
        size_t p_period = json_str.find("\"regular\"");
        if (p_period != std::string::npos) {
            size_t p_end = json_str.find("\"end\"", p_period);
            if (p_end != std::string::npos) {
                size_t p_col = json_str.find(':', p_end);
                if (p_col != std::string::npos) {
                    try { reg_end = std::stoll(json_str.substr(p_col + 1)); } catch (...) {}
                }
            }
        }

        size_t p_close = json_str.find("\"close\":[");
        if (p_close != std::string::npos) {
            size_t p_close_start = p_close + 9;
            size_t p_close_end = json_str.find(']', p_close_start);
            if (p_close_end != std::string::npos) {
                size_t i = p_close_end;
                while (i > p_close_start) {
                    --i;
                    char ch = json_str[i];
                    if ((ch >= '0' && ch <= '9') || ch == '.') {
                        size_t num_end = i + 1;
                        while (i > p_close_start && ((json_str[i-1] >= '0' && json_str[i-1] <= '9') || json_str[i-1] == '.' || json_str[i-1] == '-')) {
                            --i;
                        }
                        size_t num_start = i;
                        try {
                            double last_close = std::stod(json_str.substr(num_start, num_end - num_start));
                            if (last_close > 0.0) {
                                int64_t last_ts = 0;
                                size_t p_ts = json_str.find("\"timestamp\":[");
                                if (p_ts != std::string::npos) {
                                    size_t p_ts_start = p_ts + 13;
                                    size_t p_ts_end = json_str.find(']', p_ts_start);
                                    if (p_ts_end != std::string::npos) {
                                        size_t j = p_ts_end;
                                        while (j > p_ts_start) {
                                            --j;
                                            if (json_str[j] >= '0' && json_str[j] <= '9') {
                                                size_t ts_end = j + 1;
                                                while (j > p_ts_start && json_str[j-1] >= '0' && json_str[j-1] <= '9') --j;
                                                last_ts = std::stoll(json_str.substr(j, ts_end - j));
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (last_ts > reg_end && reg_end > 0) {
                                    ah_price = last_close;
                                }
                            }
                        } catch (...) {}
                        break;
                    }
                }
            }
        }
    }

    if (!quote.is_crypto && ah_price > 0.0 && std::abs(ah_price - reg_price) > 0.001) {
        quote.aftermarket_price = ah_price;
        quote.aftermarket_change_pct = ((ah_price - reg_price) / reg_price * 100.0);
        quote.has_aftermarket = true;
        quote.formatted_aftermarket = std::format("AH ${:.2f} ({:+.2f}%)", ah_price, quote.aftermarket_change_pct);
    } else {
        quote.aftermarket_price = 0.0;
        quote.aftermarket_change_pct = 0.0;
        quote.has_aftermarket = false;
        quote.formatted_aftermarket = "";
    }

    quote.formatted_price = format_currency(quote.price, quote.is_crypto);
    quote.last_updated_epoch = (uint64_t)time(nullptr);
    return true;
}

void StockStreamer::fetch_sync() {
    if (m_fetching.exchange(true)) return;
    struct Guard { std::atomic<bool>& f; ~Guard() { f.store(false); } } g{m_fetching};

    std::vector<std::string> symbols = {"NVDA", "AAPL", "MSFT", "AMZN", "GOOGL", "META", "BRK-B", "TSLA", "AVGO", "JPM"};
    std::string crypto_sym = "BTC-USD";
    {
        std::shared_lock lk(g_config_mtx);
        if (!g_cfg.stockstreamer_symbols.empty()) {
            symbols = g_cfg.stockstreamer_symbols;
        }
        if (!g_cfg.stockstreamer_crypto.empty()) {
            crypto_sym = g_cfg.stockstreamer_crypto;
        }
    }

    // Parallel fetch for all stocks
    std::vector<std::future<std::pair<std::string, std::string>>> futures;
    for (const auto& sym : symbols) {
        std::string yahoo_sym = (sym == "BRK.B") ? "BRK-B" : sym;
        futures.push_back(std::async(std::launch::async, [sym, yahoo_sym]() {
            std::string url = std::format("https://query1.finance.yahoo.com/v8/finance/chart/{}?interval=1m&range=1d&includePrePost=true", yahoo_sym);
            return std::make_pair(sym, http_get_json(url, 4));
        }));
    }

    // Parallel fetch for crypto
    auto crypto_future = std::async(std::launch::async, [crypto_sym]() {
        std::string url = std::format("https://query1.finance.yahoo.com/v8/finance/chart/{}?interval=1m&range=1d&includePrePost=true", crypto_sym);
        return http_get_json(url, 4);
    });

    std::vector<StockQuote> fetched_stocks;
    for (auto& f : futures) {
        auto [sym, json_resp] = f.get();
        StockQuote q;
        q.symbol = sym;
        q.display_symbol = clean_display_sym(sym);
        q.name = get_company_name(sym);
        q.is_crypto = false;

        if (parse_yahoo_chart_meta(json_resp, q)) {
            fetched_stocks.push_back(std::move(q));
        } else {
            std::shared_lock lk(m_quotes_mtx);
            auto it = std::find_if(m_stocks.begin(), m_stocks.end(), [&](const StockQuote& sq) { return sq.symbol == sym; });
            if (it != m_stocks.end()) {
                fetched_stocks.push_back(*it);
            }
        }
    }

    // Process Crypto result
    std::string btc_json = crypto_future.get();
    StockQuote fetched_crypto;
    fetched_crypto.symbol = crypto_sym;
    fetched_crypto.display_symbol = "BTC";
    fetched_crypto.name = "Bitcoin";
    fetched_crypto.is_crypto = true;

    bool btc_ok = parse_yahoo_chart_meta(btc_json, fetched_crypto);
    if (!btc_ok) {
        std::string cg_url = "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true";
        std::string cg_json = http_get_json(cg_url, 4);
        if (!cg_json.empty()) {
            std::regex re_p("\"usd\"\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)");
            std::regex re_c("\"usd_24h_change\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
            std::smatch mp, mc;
            if (std::regex_search(cg_json, mp, re_p)) {
                try {
                    fetched_crypto.price = std::stod(mp[1].str());
                    if (std::regex_search(cg_json, mc, re_c)) {
                        fetched_crypto.change_pct = std::stod(mc[1].str());
                    }
                    fetched_crypto.formatted_price = format_currency(fetched_crypto.price, true);
                    fetched_crypto.last_updated_epoch = (uint64_t)time(nullptr);
                    btc_ok = true;
                } catch (...) {}
            }
        }
    }

    {
        std::unique_lock lk(m_quotes_mtx);
        if (!fetched_stocks.empty()) {
            m_stocks = std::move(fetched_stocks);
        }
        if (btc_ok) {
            m_crypto = std::move(fetched_crypto);
        }
        m_last_sync_time.store(time(nullptr));
        m_last_error.store(0);
        g_logger.info("STOCKS: Realtime sync completed for {} S&P 500 stocks + BTC", m_stocks.size());
    }
}

void StockStreamer::sync() {
    fetch_sync();
}

bool StockStreamer::start() {
    stop();
    m_running.store(true);

    {
        std::unique_lock lk(m_quotes_mtx);
        if (m_stocks.empty()) {
            std::vector<std::pair<std::string, double>> init_list = {
                {"NVDA", 208.48}, {"AAPL", 310.34}, {"MSFT", 487.31}, {"AMZN", 262.07},
                {"GOOGL", 348.06}, {"META", 559.02}, {"BRK.B", 504.32}, {"TSLA", 348.95},
                {"AVGO", 358.76}, {"JPM", 356.39}
            };
            for (const auto& [sym, pr] : init_list) {
                StockQuote q;
                q.symbol = sym;
                q.display_symbol = clean_display_sym(sym);
                q.name = get_company_name(sym);
                q.price = pr;
                q.change = (sym == "NVDA" || sym == "TSLA" || sym == "AVGO") ? -6.24 : 1.20;
                q.change_pct = (sym == "NVDA" || sym == "TSLA" || sym == "AVGO") ? -1.85 : 1.15;
                q.formatted_price = format_currency(pr, false);
                q.is_crypto = false;
                m_stocks.push_back(std::move(q));
            }
            m_crypto.symbol = "BTC-USD";
            m_crypto.display_symbol = "BTC";
            m_crypto.name = "Bitcoin";
            m_crypto.price = 79820.0;
            m_crypto.change_pct = 1.07;
            m_crypto.formatted_price = "$79,820.00";
            m_crypto.is_crypto = true;
        }
    }

    if (!spawn_thread_safe(m_worker_thread, "stockstreamer", [this]() {
        this->sync();

        while (this->m_running.load()) {
            int refresh_secs = 10;
            {
                std::shared_lock lk(g_config_mtx);
                refresh_secs = std::clamp(g_cfg.stockstreamer_refresh_seconds, 5, 300);
            }
            for (int i = 0; i < refresh_secs && this->m_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (this->m_running.load()) {
                this->sync();
            }
        }
    })) {
        g_logger.error("STOCKS: Failed to spawn background worker thread");
        m_running.store(false);
        return false;
    }
    return true;
}

void StockStreamer::stop() {
    m_running.store(false);
}

std::vector<StockQuote> StockStreamer::get_stocks() const {
    std::shared_lock lk(m_quotes_mtx);
    return m_stocks;
}

StockQuote StockStreamer::get_crypto() const {
    std::shared_lock lk(m_quotes_mtx);
    return m_crypto;
}

std::string StockStreamer::get_status_json() const {
    std::shared_lock lk(m_quotes_mtx);
    std::ostringstream ss;
    ss << "{\"last_sync\":" << m_last_sync_time.load()
       << ",\"last_error\":" << m_last_error.load()
       << ",\"stocks\":[";
    for (size_t i = 0; i < m_stocks.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"symbol\":\"" << m_stocks[i].display_symbol << "\""
           << ",\"price\":" << m_stocks[i].price
           << ",\"change\":" << m_stocks[i].change
           << ",\"change_pct\":" << m_stocks[i].change_pct
           << ",\"aftermarket_price\":" << m_stocks[i].aftermarket_price
           << ",\"aftermarket_change_pct\":" << m_stocks[i].aftermarket_change_pct
           << ",\"has_aftermarket\":" << (m_stocks[i].has_aftermarket ? "true" : "false") << "}";
    }
    ss << "],\"crypto\":{\"symbol\":\"BTC\""
       << ",\"price\":" << m_crypto.price
       << ",\"change_pct\":" << m_crypto.change_pct << "}}";
    return ss.str();
}

void StockStreamer::render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w) {
    if (!renderer || !font_renderer || bounds.h <= 0 || bounds.w <= 0) return;

    // 1. Glassmorphic card backdrop
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 12, 16, 26, 240);
    SDL_FRect bg_rect = { (float)bounds.x, (float)bounds.y, (float)bounds.w, (float)bounds.h };
    SDL_RenderFillRect(renderer, &bg_rect);

    // Left divider accent border (Electric Cyan)
    SDL_SetRenderDrawColor(renderer, 0, 200, 255, 140);
    SDL_FRect left_border = { (float)bounds.x, (float)bounds.y, 2.0f, (float)bounds.h };
    SDL_RenderFillRect(renderer, &left_border);

    // Top horizontal accent divider
    SDL_SetRenderDrawColor(renderer, 0, 200, 255, 160);
    SDL_FRect top_div = { (float)bounds.x, (float)bounds.y, (float)bounds.w, 1.0f };
    SDL_RenderFillRect(renderer, &top_div);

    int title_font_size  = (int)round(12.0 * screen_w / 1920.0);
    int col_hdr_size     = (int)round(9.0 * screen_w / 1920.0);
    int row_font_size    = (int)round(10.0 * screen_w / 1920.0);

    FontHandle& title_font = font_renderer->load_font(font_path, title_font_size);
    FontHandle& col_font   = font_renderer->load_font(font_path, col_hdr_size);
    FontHandle& row_font   = font_renderer->load_font(font_path, row_font_size);

    int pad_x = bounds.x + (int)round(10.0 * screen_w / 1920.0);
    int cur_y = bounds.y + (int)round(8.0 * screen_w / 1920.0);

    // Tightly-packed column positions (calibrated inside physical matte X <= 1860)
    int x_stock = pad_x;
    int x_price = pad_x + (int)round(38.0 * screen_w / 1920.0);
    int x_chg   = pad_x + (int)round(94.0 * screen_w / 1920.0);
    int x_pct   = pad_x + (int)round(144.0 * screen_w / 1920.0);
    int x_ah    = pad_x + (int)round(198.0 * screen_w / 1920.0);
    int usable_w = bounds.w - (pad_x - bounds.x) * 2;

    // Section 1 Header: STOCKS / S&P 500 TOP 10
    int dot_r = (int)round(3.0 * screen_w / 1920.0);
    SDL_FRect dot_rect = { (float)pad_x, (float)(cur_y + 2), (float)(dot_r * 2), (float)(dot_r * 2) };
    SDL_SetRenderDrawColor(renderer, 140, 150, 165, 255);
    SDL_RenderFillRect(renderer, &dot_rect);

    font_renderer->draw_text(pad_x + dot_r * 2 + 5, cur_y, title_font, "STOCKS  •  S&P 500 TOP 10", 255, 255, 255, 255);
    cur_y += title_font_size + 6;

    // Column Labels
    font_renderer->draw_text(x_stock, cur_y, col_font, "STOCK",  140, 165, 190, 240);
    font_renderer->draw_text(x_price, cur_y, col_font, "PRICE",  140, 165, 190, 240);
    font_renderer->draw_text(x_chg,   cur_y, col_font, "CHANGE", 140, 165, 190, 240);
    font_renderer->draw_text(x_pct,   cur_y, col_font, "%",      140, 165, 190, 240);
    font_renderer->draw_text(x_ah,    cur_y, col_font, "AH %",   140, 165, 190, 240);
    cur_y += col_hdr_size + 3;

    // Header divider line
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 45);
    SDL_FRect hdr_div = { (float)pad_x, (float)cur_y, (float)usable_w, 1.0f };
    SDL_RenderFillRect(renderer, &hdr_div);
    cur_y += 2;

    // Copy quotes thread-safely
    std::vector<StockQuote> stocks_copy;
    StockQuote crypto_copy;
    {
        std::shared_lock lk(m_quotes_mtx);
        stocks_copy = m_stocks;
        crypto_copy = m_crypto;
    }

    // 2. Render Top 10 Stocks (tightly packed with separator lines)
    int row_h = (int)round(18.0 * screen_w / 1920.0);
    int max_y_stocks = bounds.y + bounds.h - (int)round(55.0 * screen_w / 1920.0);

    for (size_t idx = 0; idx < stocks_copy.size(); ++idx) {
        const auto& st = stocks_copy[idx];
        if (cur_y + row_h > max_y_stocks) break;

        // Symbol
        font_renderer->draw_text(x_stock, cur_y + 1, row_font, st.display_symbol, 240, 245, 255, 255);

        // Price
        font_renderer->draw_text(x_price, cur_y + 1, row_font, st.formatted_price, 255, 255, 255, 255);

        // Change ($)
        std::string chg_str = std::format("{:+.2f}", st.change);
        if (st.change >= 0) {
            font_renderer->draw_text(x_chg, cur_y + 1, row_font, chg_str, 0, 230, 118, 255);
        } else {
            font_renderer->draw_text(x_chg, cur_y + 1, row_font, chg_str, 255, 82, 82, 255);
        }

        // %
        std::string pct_str = std::format("{:+.2f}%", st.change_pct);
        if (st.change_pct >= 0) {
            font_renderer->draw_text(x_pct, cur_y + 1, row_font, pct_str, 0, 230, 118, 255);
        } else {
            font_renderer->draw_text(x_pct, cur_y + 1, row_font, pct_str, 255, 82, 82, 255);
        }

        // AH %
        if (st.has_aftermarket) {
            std::string ah_str = std::format("{:+.2f}%", st.aftermarket_change_pct);
            font_renderer->draw_text(x_ah, cur_y + 1, row_font, ah_str, 255, 205, 100, 255);
        } else {
            font_renderer->draw_text(x_ah, cur_y + 1, row_font, "--", 130, 140, 155, 200);
        }

        cur_y += row_h;

        // Horizontal separator
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 16);
        SDL_FRect row_div = { (float)pad_x, (float)cur_y, (float)usable_w, 1.0f };
        SDL_RenderFillRect(renderer, &row_div);
        cur_y += 1;
    }

    cur_y += 3;

    // 3. CRYPTO Section divider
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 30);
    SDL_FRect crypto_div = { (float)pad_x, (float)cur_y, (float)usable_w, 1.0f };
    SDL_RenderFillRect(renderer, &crypto_div);
    cur_y += 5;

    // Crypto Header (Slate Grey box, matching stocks)
    SDL_FRect c_dot = { (float)pad_x, (float)(cur_y + 2), (float)(dot_r * 2), (float)(dot_r * 2) };
    SDL_SetRenderDrawColor(renderer, 140, 150, 165, 255);
    SDL_RenderFillRect(renderer, &c_dot);

    font_renderer->draw_text(pad_x + dot_r * 2 + 5, cur_y, title_font, "CRYPTO", 255, 255, 255, 255);
    cur_y += title_font_size + 5;

    // 4. Bitcoin Row (single line, condensed to fit without overlap)
    // Use a smaller font for BTC price to avoid overlap in the tight columns
    int btc_font_size = (int)round(9.0 * screen_w / 1920.0);
    FontHandle& btc_font = font_renderer->load_font(font_path, btc_font_size);

    // Symbol: BTC (Amber Gold)
    font_renderer->draw_text(x_stock, cur_y + 1, row_font, "BTC", 255, 180, 50, 255);

    // Price (use smaller font to avoid overlap with change column)
    std::string btc_price_str = !crypto_copy.formatted_price.empty() ? crypto_copy.formatted_price : "$79,820.00";
    font_renderer->draw_text(x_price, cur_y + 1, btc_font, btc_price_str, 255, 255, 255, 255);

    // 24h Change ($)
    double btc_chg_val = (crypto_copy.prev_close > 0) ? (crypto_copy.price - crypto_copy.prev_close) : (crypto_copy.price * crypto_copy.change_pct / 100.0);
    std::string btc_chg_str = std::format("{:+.0f}", btc_chg_val);
    if (btc_chg_val >= 0) {
        font_renderer->draw_text(x_chg, cur_y + 1, btc_font, btc_chg_str, 0, 230, 118, 255);
    } else {
        font_renderer->draw_text(x_chg, cur_y + 1, btc_font, btc_chg_str, 255, 82, 82, 255);
    }

    // 24h %
    std::string btc_pct = std::format("{:+.2f}%", crypto_copy.change_pct);
    if (crypto_copy.change_pct >= 0) {
        font_renderer->draw_text(x_pct, cur_y + 1, btc_font, btc_pct, 0, 230, 118, 255);
    } else {
        font_renderer->draw_text(x_pct, cur_y + 1, btc_font, btc_pct, 255, 82, 82, 255);
    }

    // AH % (N/A for crypto)
    font_renderer->draw_text(x_ah, cur_y + 1, btc_font, "--", 130, 140, 155, 200);

    cur_y += row_h;
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 16);
    SDL_FRect btc_div = { (float)pad_x, (float)cur_y, (float)usable_w, 1.0f };
    SDL_RenderFillRect(renderer, &btc_div);
}

