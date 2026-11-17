#pragma once
#include <string>
#include <vector>

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
