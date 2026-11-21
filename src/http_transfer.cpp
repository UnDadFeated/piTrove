#include "http_transfer.h"
#include "file_ops.h"
#include <curl/curl.h>
#include <limits>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>

namespace http_transfer {
bool is_google_media_url(const std::string& url) {
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char* scheme = nullptr;
    char* hostname = nullptr;
    bool valid = curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
                 curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                 curl_url_get(parsed, CURLUPART_HOST, &hostname, 0) == CURLUE_OK;
    if (valid) {
        std::string host(hostname);
        std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
        valid = std::string_view(scheme) == "https" &&
                (host == "googleusercontent.com" || host.ends_with(".googleusercontent.com") ||
                 host == "ggpht.com" || host.ends_with(".ggpht.com"));
    }
    curl_free(scheme); curl_free(hostname); curl_url_cleanup(parsed);
    return valid;
}
size_t append_text(void* data, size_t size, size_t count, void* opaque) noexcept {
    auto& buffer = *static_cast<TextBuffer*>(opaque);
    if (size && count > std::numeric_limits<size_t>::max() / size) return 0;
    size_t bytes = size * count;
    if (bytes > buffer.limit || buffer.text->size() > buffer.limit - bytes) return 0;
    try { buffer.text->append(static_cast<char*>(data), bytes); } catch (...) { return 0; }
    return bytes;
}
static curl_socket_t open_public_socket(void*, curlsocktype purpose, struct curl_sockaddr* address) {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    if (address->family == AF_INET && address->addrlen >= sizeof(sockaddr_in)) {
        const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(&address->addr)->sin_addr.s_addr);
        if (ip >> 24 == 0 || ip >> 24 == 10 || ip >> 24 == 127 || ip >> 24 >= 224 ||
            ip >> 22 == 0x191 || ip >> 16 == 0xa9fe || ip >> 20 == 0xac1 ||
            ip >> 8 == 0xc00000 || ip >> 8 == 0xc00002 || ip >> 8 == 0xc05863 ||
            ip >> 16 == 0xc0a8 || ip >> 17 == 0x6309 ||
            ip >> 8 == 0xc63364 || ip >> 8 == 0xcb0071) {
            return CURL_SOCKET_BAD;
        }
    } else if (address->family == AF_INET6 && address->addrlen >= sizeof(sockaddr_in6)) {
        const auto& ip = reinterpret_cast<const sockaddr_in6*>(&address->addr)->sin6_addr.s6_addr;
        // Global unicast, excluding transition ranges that encode IPv4 endpoints.
        if ((ip[0] & 0xe0) != 0x20 || (ip[0] == 0x20 && ip[1] == 0x02) ||
            (ip[0] == 0x20 && ip[1] == 0x01 && ip[2] <= 1)) {
            return CURL_SOCKET_BAD;
        }
    } else {
        return CURL_SOCKET_BAD;
    }
    return socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
}

void configure_public_https(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, open_public_socket);
}

static void options(CURL* curl, const std::string& url) {
    configure_public_https(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
}
static bool successful(CURL* curl, CURLcode result) {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    return result == CURLE_OK && status >= 200 && status < 300;
}
bool get(const std::string& url, std::string& response, const std::string& bearer, size_t limit) {
    response.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    options(curl, url);
    TextBuffer buffer{&response, limit};
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_text);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_slist* headers = nullptr;
    if (!bearer.empty()) {
        headers = curl_slist_append(nullptr, ("Authorization: Bearer " + bearer).c_str());
        if (!headers) { curl_easy_cleanup(curl); return false; }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    CURLcode result = curl_easy_perform(curl);
    bool ok = successful(curl, result);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) response.clear();
    return ok;
}
bool post_form(const std::string& url, const std::string& body, std::string& response) {
    response.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    options(curl, url);
    TextBuffer buffer{&response, 2 * 1024 * 1024};
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_text);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    // Do not redirect a credential-bearing form body to another host.
    CURLcode result = curl_easy_perform(curl);
    bool ok = successful(curl, result);
    curl_easy_cleanup(curl);
    // Error JSON is useful to OAuth callers; partial transport responses are not.
    if (result != CURLE_OK) response.clear();
    return ok;
}
struct FileBuffer { FILE* file; size_t limit, written = 0; const std::atomic<bool>* running; };
static size_t write_file(void* data, size_t size, size_t count, void* opaque) noexcept {
    auto& buffer = *static_cast<FileBuffer*>(opaque);
    if ((buffer.running && !buffer.running->load()) || (size && count > std::numeric_limits<size_t>::max() / size)) return 0;
    size_t bytes = size * count;
    if (bytes > buffer.limit || buffer.written > buffer.limit - bytes) return 0;
    size_t written = fwrite(data, 1, bytes, buffer.file);
    buffer.written += written;
    return written;
}
static int cancelled(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* running = static_cast<const std::atomic<bool>*>(opaque);
    return running && !running->load();
}
bool download(const std::string& url, const std::string& path, size_t limit, const std::atomic<bool>* running, const std::string& bearer) {
    if (running && !running->load()) return false;
    std::string pattern = path + ".part.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd < 0) return false;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    FILE* file = fdopen(fd, "wb");
    if (!file) { close(fd); unlink(name.data()); return false; }
    CURL* curl = curl_easy_init();
    bool ok = false;
    FileBuffer buffer{file, limit, 0, running};
    if (curl) {
        options(curl, url);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, bearer.empty() ? 1L : 0L);
        if (!bearer.empty()) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
            curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, bearer.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelled);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, running);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        ok = successful(curl, curl_easy_perform(curl)) && buffer.written >= 1024;
        curl_easy_cleanup(curl);
    }
    if (fflush(file) != 0 || fsync(fd) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok) {
        std::error_code error;
        ok = move_without_replace(name.data(), path, error);
    }
    if (!ok) unlink(name.data());
    return ok;
}
}
