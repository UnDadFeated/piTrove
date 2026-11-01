#pragma once
#include <string>
#include <atomic>
#include <cstddef>
namespace http_transfer {
bool is_google_media_url(const std::string& url);
bool get(const std::string& url, std::string& response, const std::string& bearer = "", size_t limit = 4 * 1024 * 1024);
bool post_form(const std::string& url, const std::string& body, std::string& response);
bool download(const std::string& url, const std::string& path, size_t limit, const std::atomic<bool>* running = nullptr);
// Use this callback for bounded text responses; it never lets C++ exceptions cross libcurl.
struct TextBuffer { std::string* text; size_t limit; };
size_t append_text(void* data, size_t size, size_t count, void* opaque) noexcept;
}
