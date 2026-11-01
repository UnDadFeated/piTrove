#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <charconv>

namespace config_text {
inline std::string strip_comments(std::string_view line) {
    char quote = 0;
    bool escape = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (escape) { escape = false; continue; }
        if (quote == '"' && c == '\\') { escape = true; continue; }
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '#' || c == ';') return std::string(line.substr(0, i));
    }
    return std::string(line);
}
inline std::string escape(std::string_view text) {
    std::string out;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
                } else out += char(c);
        }
    }
    return out;
}
inline std::optional<std::string> decode(std::string_view value) {
    if (value.size() < 2 || (value.front() != '"' && value.front() != '\'')) return std::nullopt;
    const char quote = value.front();
    if (value.back() != quote) return std::nullopt;
    std::string out;
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        char c = value[i];
        if (c == quote) return std::nullopt;
        if (quote == '\'' || c != '\\') { out += c; continue; }
        if (++i + 1 >= value.size()) return std::nullopt;
        switch (value[i]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': case 'U': {
                size_t count = value[i] == 'u' ? 4 : 8;
                if (i + count + 1 >= value.size()) return std::nullopt;
                unsigned int cp = 0;
                auto [end, ec] = std::from_chars(value.data() + i + 1, value.data() + i + count + 1, cp, 16);
                if (ec != std::errc{} || end != value.data() + i + count + 1 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return std::nullopt;
                if (cp <= 0x7f) out += char(cp);
                else if (cp <= 0x7ff) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                else if (cp <= 0xffff) { out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                else { out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                i += count;
                break;
            }
            default: return std::nullopt;
        }
    }
    return out;
}
inline std::optional<std::vector<std::string>> array(std::string_view value) {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return std::nullopt;
    std::vector<std::string> out;
    size_t pos = 1;
    while (pos + 1 < value.size()) {
        while (pos + 1 < value.size() && (value[pos] == ' ' || value[pos] == '\t')) ++pos;
        if (pos + 1 >= value.size()) break;
        const size_t start = pos;
        const char quote = value[pos++];
        if (quote != '"' && quote != '\'') return std::nullopt;
        bool escaped = false, closed = false;
        while (pos + 1 < value.size()) {
            char c = value[pos++];
            if (escaped) { escaped = false; continue; }
            if (quote == '"' && c == '\\') { escaped = true; continue; }
            if (c == quote) { closed = true; break; }
        }
        if (!closed) return std::nullopt;
        auto item = decode(value.substr(start, pos - start));
        if (!item) return std::nullopt;
        out.push_back(std::move(*item));
        while (pos + 1 < value.size() && (value[pos] == ' ' || value[pos] == '\t')) ++pos;
        if (pos + 1 < value.size() && value[pos++] != ',') return std::nullopt;
    }
    return out;
}
}
