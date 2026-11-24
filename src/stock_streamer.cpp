#include "stock_streamer.h"
#include "http_transfer.h"
#include "config.h"
#include "config_text.h"
#include "http_protocol.h"
#include "infopanel_render.h"
#include "util.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>
#include <format>
#include <cmath>
#include <chrono>
#include <memory>

extern Config g_cfg;
extern std::shared_mutex g_config_mtx;
extern Logger g_logger;

namespace {

bool us_market_open(std::chrono::system_clock::time_point now) {
    using namespace std::chrono;
    try {
        const auto local = locate_zone("America/New_York")->to_local(now);
        const auto date = floor<days>(local);
        const auto weekday_num = weekday(sys_days(date.time_since_epoch())).iso_encoding();
        const auto time = local - date;
        return weekday_num <= 5 && time >= hours(9) + minutes(30) && time < hours(16);
    } catch (...) {
        return false;
    }
}

struct QuoteRequest {
    std::string url;
    std::string body;
    http_transfer::TextBuffer buffer{&body, 256 * 1024};
    CURL* handle = nullptr;
    bool success = false;
};

void fetch_quotes(std::vector<QuoteRequest>& requests, const std::atomic<bool>* running) {
    if (running && !running->load()) return;
    const std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    if (!multi) return;
    curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, 6L);
    for (auto& request : requests) {
        auto* curl = curl_easy_init();
        if (!curl) continue;
        http_transfer::configure_public_https(curl);
        http_transfer::set_cancel_flag(curl, running);
        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_transfer::append_text);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &request.buffer);
        curl_easy_setopt(curl, CURLOPT_PRIVATE, &request);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "piTrove/" VERSION);
        if (curl_multi_add_handle(multi.get(), curl) != CURLM_OK) curl_easy_cleanup(curl);
        else request.handle = curl;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    int active = 0;
    while ((!running || running->load()) &&
           curl_multi_perform(multi.get(), &active) == CURLM_OK && active > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        if (curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr) != CURLM_OK) break;
    }
    int queued = 0;
    while (const auto* message = curl_multi_info_read(multi.get(), &queued)) {
        if (message->msg != CURLMSG_DONE) continue;
        QuoteRequest* request = nullptr;
        long status = 0;
        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
        curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &status);
        if (request) request->success = message->data.result == CURLE_OK && status == 200;
    }
    for (auto& request : requests) {
        if (!request.handle) continue;
        curl_multi_remove_handle(multi.get(), request.handle);
        curl_easy_cleanup(request.handle);
    }
}

} // namespace

static std::string format_currency(double val, bool is_crypto = false) {
    if (!std::isfinite(val) || val < 0 || val > 1e15) return "—";
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

static double json_number(const nlohmann::json& obj, const char* key, double fallback = 0.0) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return fallback;
    const double val = it->get<double>();
    return std::isfinite(val) ? val : fallback;
}

static bool parse_yahoo_chart_meta(const std::string& json_str, StockQuote& quote) {
    if (json_str.empty()) return false;
    const auto doc = nlohmann::json::parse(json_str, nullptr, false);
    if (!doc.is_object()) return false;
    const auto chart = doc.find("chart");
    if (chart == doc.end() || !chart->is_object()) return false;
    const auto results = chart->find("result");
    if (results == chart->end() || !results->is_array() || results->empty() || !(*results)[0].is_object()) return false;
    const auto& result = (*results)[0];
    const auto meta_it = result.find("meta");
    if (meta_it == result.end() || !meta_it->is_object()) return false;
    const auto& meta = *meta_it;

    const double reg_price = json_number(meta, "regularMarketPrice", 0.0);
    double prev_close = json_number(meta, "chartPreviousClose", 0.0);
    if (prev_close <= 0.0) prev_close = json_number(meta, "previousClose", 0.0);

    if (!std::isfinite(reg_price) || reg_price <= 0.0) return false;

    quote.price = reg_price;
    quote.prev_close = prev_close;
    quote.change = (prev_close > 0.0) ? (reg_price - prev_close) : 0.0;
    quote.change_pct = (prev_close > 0.0) ? ((reg_price - prev_close) / prev_close * 100.0) : 0.0;

    const double post_price = json_number(meta, "postMarketPrice", 0.0);
    const double pre_price = json_number(meta, "preMarketPrice", 0.0);
    double ah_price = (post_price > 0.0) ? post_price : ((pre_price > 0.0) ? pre_price : 0.0);

    // Traditional stocks: fallback to latest extended-hours candle from chart 1m series (Crypto is 24/7 continuous spot)
    if (!quote.is_crypto && ah_price <= 0.0) {
        int64_t reg_end = 0;
        if (meta.contains("currentTradingPeriod") && meta["currentTradingPeriod"].is_object()) {
            const auto& tp = meta["currentTradingPeriod"];
            if (tp.contains("regular") && tp["regular"].is_object() && tp["regular"].contains("end") && tp["regular"]["end"].is_number_integer()) {
                reg_end = tp["regular"]["end"].get<int64_t>();
            }
        }
        if (reg_end > 0 && result.contains("timestamp") && result["timestamp"].is_array() &&
            result.contains("indicators") && result["indicators"].is_object() &&
            result["indicators"].contains("quote") && result["indicators"]["quote"].is_array() &&
            !result["indicators"]["quote"].empty() && result["indicators"]["quote"][0].is_object() &&
            result["indicators"]["quote"][0].contains("close") && result["indicators"]["quote"][0]["close"].is_array()) {
            const auto& timestamps = result["timestamp"];
            const auto& closes = result["indicators"]["quote"][0]["close"];
            const size_t n = std::min(timestamps.size(), closes.size());
            for (size_t i = n; i > 0; --i) {
                const auto& c = closes[i - 1];
                const auto& ts = timestamps[i - 1];
                if (c.is_number() && ts.is_number_integer()) {
                    const double last_close = c.get<double>();
                    const int64_t last_ts = ts.get<int64_t>();
                    if (std::isfinite(last_close) && last_close > 0.0 && last_ts > reg_end) {
                        ah_price = last_close;
                    }
                    break;
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

    const bool market_open = us_market_open(std::chrono::system_clock::now());
    std::vector<StockQuote> old_stocks;
    {
        std::shared_lock lock(m_quotes_mtx);
        old_stocks = m_stocks;
    }
    std::vector<QuoteRequest> requests(market_open ? symbols.size() + 1 : 1);
    const auto chart = [](const std::string& symbol) {
        return "https://query1.finance.yahoo.com/v8/finance/chart/" +
            http_protocol::url_encode(symbol == "BRK.B" ? "BRK-B" : symbol) +
            "?interval=1m&range=1d";
    };
    for (size_t i = 0; market_open && i < symbols.size(); ++i) requests[i].url = chart(symbols[i]);
    requests.back().url = chart(crypto_sym);
    fetch_quotes(requests, &m_allow_io);

    std::vector<StockQuote> fetched_stocks;
    bool fresh_stock_quote = false;
    for (size_t i = 0; i < symbols.size(); ++i) {
        const auto& sym = symbols[i];
        const auto previous = std::find_if(old_stocks.begin(), old_stocks.end(),
            [&](const StockQuote& quote) { return quote.symbol == sym; });
        if (!market_open || !requests[i].success) {
            if (previous != old_stocks.end()) fetched_stocks.push_back(*previous);
            continue;
        }
        const auto& json_resp = requests[i].body;
        StockQuote q;
        q.symbol = sym;
        q.display_symbol = clean_display_sym(sym);
        q.name = get_company_name(sym);
        q.is_crypto = false;

        if (parse_yahoo_chart_meta(json_resp, q)) {
            fresh_stock_quote = true;
            fetched_stocks.push_back(std::move(q));
        } else if (previous != old_stocks.end()) {
            fetched_stocks.push_back(*previous);
        }
    }

    const auto& btc_json = requests.back().body;
    StockQuote fetched_crypto;
    fetched_crypto.symbol = crypto_sym;
    fetched_crypto.display_symbol = crypto_sym.ends_with("-USD")
        ? crypto_sym.substr(0, crypto_sym.size() - 4) : crypto_sym;
    fetched_crypto.name = get_company_name(crypto_sym);
    fetched_crypto.is_crypto = true;

    bool btc_ok = requests.back().success && parse_yahoo_chart_meta(btc_json, fetched_crypto);
    if (!btc_ok && (crypto_sym == "BTC-USD" || crypto_sym == "BTC")) {
        std::string cg_url = "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true";
        std::vector<QuoteRequest> fallback(1);
        fallback.front().url = std::move(cg_url);
        fetch_quotes(fallback, &m_allow_io);
        const auto& cg_json = fallback.front().body;
        if (fallback.front().success && !cg_json.empty()) {
            const auto doc = nlohmann::json::parse(cg_json, nullptr, false);
            if (doc.is_object() && doc.contains("bitcoin") && doc["bitcoin"].is_object()) {
                const auto& btc = doc["bitcoin"];
                const double price = json_number(btc, "usd", 0.0);
                if (price > 0.0) {
                    fetched_crypto.price = price;
                    fetched_crypto.change_pct = json_number(btc, "usd_24h_change", 0.0);
                    fetched_crypto.formatted_price = format_currency(fetched_crypto.price, true);
                    fetched_crypto.last_updated_epoch = static_cast<uint64_t>(time(nullptr));
                    btc_ok = true;
                }
            }
        }
    }

    {
        std::unique_lock lk(m_quotes_mtx);
        // This contains fresh results and cached quotes for the currently selected symbols.
        m_stocks = std::move(fetched_stocks);
        if (btc_ok) {
            m_crypto = std::move(fetched_crypto);
        } else if (m_crypto.symbol != crypto_sym) {
            m_crypto = {};
            m_crypto.symbol = fetched_crypto.symbol;
            m_crypto.display_symbol = fetched_crypto.display_symbol;
        }
        if (fresh_stock_quote || btc_ok) {
            m_last_sync_time.store(time(nullptr));
            m_last_error.store(0);
            g_logger.debug("STOCKS: Updated quotes for the configured symbols");
        } else {
            m_last_error.store(1);
            g_logger.warn("STOCKS: Quote refresh failed; retaining last known quotes");
        }
    }
}

void StockStreamer::sync() {
    fetch_sync();
}

bool StockStreamer::start() {
    std::lock_guard lifecycle(m_lifecycle_mtx);
    m_running.store(false);
    m_allow_io.store(false);
    if (m_worker_thread.joinable()) m_worker_thread.join();
    m_allow_io.store(true);
    m_running.store(true);

    if (!spawn_thread_safe(m_worker_thread, "stockstreamer", [this]() {
        if (!g_screen_blanked.load()) this->sync();

        while (this->m_running.load()) {
            int refresh_secs = 10;
            {
                std::shared_lock lk(g_config_mtx);
                refresh_secs = std::clamp(g_cfg.stockstreamer_refresh_seconds, 5, 300);
            }
            if (!us_market_open(std::chrono::system_clock::now()))
                refresh_secs = std::max(refresh_secs, 60);
            for (int i = 0; i < refresh_secs && this->m_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (this->m_running.load() && !g_screen_blanked.load()) {
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
    std::lock_guard lifecycle(m_lifecycle_mtx);
    m_running.store(false);
    m_allow_io.store(false);
    if (m_worker_thread.joinable()) m_worker_thread.join();
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
    auto finite_or_zero = [](double v) { return std::isfinite(v) ? v : 0.0; };
    std::shared_lock lk(m_quotes_mtx);
    std::ostringstream ss;
    ss << "{\"last_sync\":" << m_last_sync_time.load()
       << ",\"last_error\":" << m_last_error.load()
       << ",\"stocks\":[";
    for (size_t i = 0; i < m_stocks.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"symbol\":\"" << config_text::escape(m_stocks[i].display_symbol) << "\""
           << ",\"price\":" << finite_or_zero(m_stocks[i].price)
           << ",\"change\":" << finite_or_zero(m_stocks[i].change)
           << ",\"change_pct\":" << finite_or_zero(m_stocks[i].change_pct)
           << ",\"aftermarket_price\":" << finite_or_zero(m_stocks[i].aftermarket_price)
           << ",\"aftermarket_change_pct\":" << finite_or_zero(m_stocks[i].aftermarket_change_pct)
           << ",\"has_aftermarket\":" << (m_stocks[i].has_aftermarket ? "true" : "false") << "}";
    }
    ss << "],\"crypto\":{\"symbol\":\"" << config_text::escape(m_crypto.display_symbol) << "\""
       << ",\"price\":" << finite_or_zero(m_crypto.price)
       << ",\"change_pct\":" << finite_or_zero(m_crypto.change_pct)
       << ",\"available\":" << (m_crypto.last_updated_epoch > 0 ? "true" : "false") << "}}";
    return ss.str();
}

void StockStreamer::render(SDL_Renderer* renderer, FontRenderer* font_renderer, const std::string& font_path, const SDL_Rect& bounds, int screen_w) {
    if (!renderer || !font_renderer || bounds.h <= 0 || bounds.w <= 0) return;

    const InfopanelClip clip(renderer, bounds);
    if (!clip) return;

    // 1. Card backdrop
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

    const auto title_font_owner = font_renderer->load_font(font_path, title_font_size);
    FontHandle& title_font = *title_font_owner;
    const auto col_font_owner = font_renderer->load_font(font_path, col_hdr_size);
    FontHandle& col_font = *col_font_owner;
    const auto row_font_owner = font_renderer->load_font(font_path, row_font_size);
    FontHandle& row_font = *row_font_owner;

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

    font_renderer->draw_text(pad_x + dot_r * 2 + 5, cur_y, title_font, "STOCKS", 255, 255, 255, 255);
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

    if (stocks_copy.empty()) {
        font_renderer->draw_text(pad_x, cur_y + 3, row_font, "Waiting for stock quotes",
                                 140, 165, 190, 240);
        cur_y += panel_px(18, screen_w);
    }

    // 2. Render stock quotes (tightly packed with separator lines)
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
    const auto btc_font_owner = font_renderer->load_font(font_path, btc_font_size);
    FontHandle& btc_font = *btc_font_owner;

    const auto symbol = crypto_copy.display_symbol.empty() ? "—" : crypto_copy.display_symbol;
    font_renderer->draw_text(x_stock, cur_y + 1, row_font,
                             font_renderer->ellipsize(row_font, symbol, x_price - x_stock - 2),
                             255, 180, 50, 255);

    if (crypto_copy.last_updated_epoch == 0) {
        font_renderer->draw_text(x_price, cur_y + 1, btc_font, "Quote unavailable", 140, 165, 190, 240);
        return;
    }
    font_renderer->draw_text(x_price, cur_y + 1, btc_font, crypto_copy.formatted_price,
                             255, 255, 255, 255);

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

