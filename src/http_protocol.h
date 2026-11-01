#pragma once
#include <string>
#include <chrono>
#include <string_view>
#include <unordered_map>
#include <optional>

namespace http_protocol {
struct Request {
    std::string method, target, path;
    std::unordered_map<std::string, std::string> headers;
    size_t content_length = 0;
};
enum class Result { complete, incomplete, bad_request, too_large, timeout, closed };
Result parse(std::string_view raw, Request& request, size_t& required);
Result receive(int fd, std::string& raw, Request& request, int timeout_ms);
using Deadline = std::chrono::steady_clock::time_point;
bool send_until(int fd, std::string_view bytes, Deadline deadline);
bool send_all(int fd, std::string_view bytes, int timeout_ms = 2000);
std::optional<std::string> query_param(std::string_view raw, std::string_view key);
std::string url_encode(std::string_view value);
}
