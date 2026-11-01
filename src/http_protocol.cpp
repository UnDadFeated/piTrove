#include "http_protocol.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <climits>
#include <poll.h>
#include <sys/socket.h>

namespace http_protocol {
static constexpr size_t MAX_HEADERS = 8192;
static constexpr size_t MAX_BODY = 65536;
static bool token(unsigned char c) {
    return std::isalnum(c) || std::string_view("!#$%&'*+-.^_`|~").find(char(c)) != std::string_view::npos;
}
Result parse(std::string_view raw, Request& request, size_t& required) {
    required = 0;
    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) return raw.size() >= MAX_HEADERS ? Result::too_large : Result::incomplete;
    if (header_end + 4 > MAX_HEADERS) return Result::too_large;
    Request parsed;
    const size_t line_end = raw.find("\r\n");
    auto line = raw.substr(0, line_end);
    size_t first = line.find(' '), second = first == line.npos ? line.npos : line.find(' ', first + 1);
    if (first == line.npos || second == line.npos || first == 0 || second <= first + 1) return Result::bad_request;
    auto method = line.substr(0, first), target = line.substr(first + 1, second - first - 1), version = line.substr(second + 1);
    if (!std::all_of(method.begin(), method.end(), token) || target.front() != '/' ||
        (version != "HTTP/1.1" && version != "HTTP/1.0")) return Result::bad_request;
    for (unsigned char c : target) if (c <= 0x20 || c == 0x7f) return Result::bad_request;
    parsed.method = method; parsed.target = target; parsed.path = target.substr(0, target.find('?'));
    size_t pos = line_end + 2;
    while (pos < header_end) {
        size_t end = raw.find("\r\n", pos);
        if (end == raw.npos || end > header_end) return Result::bad_request;
        auto header = raw.substr(pos, end - pos);
        size_t colon = header.find(':');
        if (colon == header.npos || colon == 0 || !std::all_of(header.begin(), header.begin() + colon, token)) return Result::bad_request;
        std::string name(header.substr(0, colon));
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        auto value = header.substr(colon + 1);
        size_t start = value.find_first_not_of(" \t");
        value = start == value.npos ? std::string_view{} : value.substr(start);
        if (!value.empty()) value = value.substr(0, value.find_last_not_of(" \t") + 1);
        for (unsigned char c : value) if ((c < 0x20 && c != '\t') || c == 0x7f) return Result::bad_request;
        if (parsed.headers.contains(name) && (name == "content-length" || name == "authorization" || name == "host")) return Result::bad_request;
        parsed.headers.emplace(std::move(name), value);
        pos = end + 2;
    }
    if (parsed.headers.contains("transfer-encoding")) return Result::bad_request; // no chunked request support
    if (auto it = parsed.headers.find("content-length"); it != parsed.headers.end()) {
        const auto& value = it->second;
        auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed.content_length);
        if (ec != std::errc{} || end != value.data() + value.size()) return Result::bad_request;
        if (parsed.content_length > MAX_BODY) return Result::too_large;
    }
    required = header_end + 4 + parsed.content_length;
    request = std::move(parsed);
    return raw.size() >= required ? Result::complete : Result::incomplete;
}
using Clock = std::chrono::steady_clock;
static int wait_fd(int fd, short events, Clock::time_point deadline) {
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) return 0;
        pollfd pfd{fd, events, 0};
        int ready = poll(&pfd, 1, int(std::min<long long>(remaining, INT_MAX)));
        if (ready < 0 && errno == EINTR) continue;
        return ready;
    }
}
Result receive(int fd, std::string& raw, Request& request, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    raw.clear(); raw.reserve(4096);
    while (true) {
        size_t required;
        Result result = parse(raw, request, required);
        if (result != Result::incomplete) {
            if (result == Result::complete) raw.resize(required); // one request per connection
            return result;
        }
        int ready = wait_fd(fd, POLLIN, deadline);
        if (ready == 0) return Result::timeout;
        if (ready < 0) return Result::closed;
        char buffer[4096];
        size_t remaining = required ? required - raw.size() : MAX_HEADERS - raw.size();
        ssize_t count = recv(fd, buffer, std::min(sizeof(buffer), remaining), MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return Result::closed;
        raw.append(buffer, size_t(count));
    }
}
bool send_all(int fd, std::string_view bytes, int timeout_ms) {
    return send_until(fd, bytes, Clock::now() + std::chrono::milliseconds(timeout_ms));
}
bool send_until(int fd, std::string_view bytes, Deadline deadline) {
    while (!bytes.empty()) {
        if (Clock::now() >= deadline) return false;
        ssize_t count = send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT, deadline) <= 0) return false;
            continue;
        }
        if (count <= 0) return false;
        bytes.remove_prefix(size_t(count));
    }
    return true;
}
static std::string decode_form(std::string_view value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            unsigned int c = 0;
            auto [end, ec] = std::from_chars(value.data() + i + 1, value.data() + i + 3, c, 16);
            if (ec == std::errc{} && end == value.data() + i + 3) { out += char(c); i += 2; continue; }
        }
        out += value[i] == '+' ? ' ' : value[i];
    }
    return out;
}
std::optional<std::string> query_param(std::string_view raw, std::string_view key) {
    Request request; size_t required;
    if (parse(raw, request, required) != Result::complete) return std::nullopt;
    auto find = [&](std::string_view form) -> std::optional<std::string> {
        while (!form.empty()) {
            size_t end = form.find('&');
            auto pair = form.substr(0, end);
            size_t eq = pair.find('=');
            if (eq != pair.npos && decode_form(pair.substr(0, eq)) == key) return decode_form(pair.substr(eq + 1));
            if (end == form.npos) break;
            form.remove_prefix(end + 1);
        }
        return std::nullopt;
    };
    auto query = request.target.find('?');
    if (query != request.target.npos) if (auto value = find(std::string_view(request.target).substr(query + 1))) return value;
    if (request.content_length) return find(raw.substr(raw.find("\r\n\r\n") + 4, request.content_length));
    return std::nullopt;
}
std::string url_encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
}
