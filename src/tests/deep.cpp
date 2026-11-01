#include "util.h"
#include "auth.h"
#include "config.h"
#include "config_text.h"
#include "file_ops.h"
#include "http_protocol.h"
#include "http_transfer.h"
#include "oauth_state.h"
#include "organizer.h"
#include "news_ticker.h"
#include "calendar.h"
#include "stock_streamer.h"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
#include <sys/socket.h>
#include <poll.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sqlite3.h>

static std::atomic<bool> mock_network{false};
extern "C" CURLcode __real_curl_easy_perform(CURL*);
extern "C" CURLcode __wrap_curl_easy_perform(CURL* curl) {
    return mock_network ? CURLE_ABORTED_BY_CALLBACK : __real_curl_easy_perform(curl);
}

namespace fs = std::filesystem;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Temp {
    std::string path;
    Temp() { char pattern[] = "/tmp/pitrove-deep-XXXXXX"; auto p = mkdtemp(pattern); check(p, "mkdtemp failed"); path = p; }
    ~Temp() { std::error_code error; fs::remove_all(path, error); }
};
static std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
static void test_config() {
    check(safe_stod("nan", -1) == -1 && safe_stof("inf", -1) == -1, "non-finite number accepted");
    check(safe_stoi("12junk", -1) == -1 && safe_stod("3.4.5", -1) == -1, "numeric prefix accepted");
    check(safe_stod(" 12.5 \t", -1) == 12.5, "valid whitespace rejected");
    std::string text = "quotes\" slash\\ # ; newline\n tab\t control\x01 unicode café";
    check(config_text::decode("\"" + config_text::escape(text) + "\"") == text, "string escape round trip failed");
    check(config_text::decode("\"\\u263a\"") == "☺", "Unicode escape failed");
    check(!config_text::decode("\"\\ud800\""), "surrogate escape accepted");
    Temp temp;
    std::string path = temp.path + "/config.toml";
    Config config;
    config.media_dir = text; config.http_api_key = text; config.mqtt_pass = text;
    config.gcalendar_ical_url = text; config.news_local_query = text;
    config.news_blacklist = {"one,two", "quote\"#;", "back\\slash"};
    config.ignore_folders = config.news_blacklist;
    config.stockstreamer_symbols = {"AAPL", "BRK-B", "^GSPC"};
    config.mqtt_tls_enabled = true; config.mqtt_ca_cert = text;
    config.dashboard_pin = "4391"; config.web_dashboard_enabled = true;
    check(config.save(path), "config save failed");
    check(config.dashboard_pin.empty() && !config.pin_hash.empty(), "PIN migration did not update memory");
    check(pitrove::auth::verify_configured_pin("4391", "", config.pin_hash), "saved PIN did not verify");
    check(!pitrove::auth::verify_configured_pin("wrong", "", config.pin_hash), "hash PIN bypassed");
    check(pitrove::auth::verify_configured_pin("anything", "", ""), "no-PIN mode changed");
    std::string saved = read_file(path);
    check(saved.find("dashboard_pin = \"4391\"") == saved.npos, "plaintext PIN persisted");
    Config loaded;
    check(loaded.load(path), "config reload failed");
    check(loaded.media_dir == text, "media path changed on reload");
    check(loaded.http_api_key == text, "remote API key changed on reload");
    check(loaded.mqtt_pass == text, "MQTT password changed on reload");
    check(loaded.news_local_query == text, "news query changed on reload");
    check(loaded.news_blacklist == config.news_blacklist && loaded.ignore_folders == config.ignore_folders, "array round trip failed");
    check(loaded.stockstreamer_symbols == config.stockstreamer_symbols, "custom stock bundle dropped on reload");
    check(loaded.mqtt_tls_enabled && loaded.mqtt_ca_cert == text, "MQTT TLS settings dropped");
    check(loaded.pin_changed && loaded.pin_hash == config.pin_hash && loaded.web_dashboard_enabled, "PIN state dropped on reload");
    config.dashboard_pin = "9284";
    check(!config.save(temp.path + "/missing/config.toml") && config.dashboard_pin == "9284", "failed save changed live PIN");
    std::ofstream(path) << "[paths]\nmedia_dir = \"a\\\"#;b\" # comment\n";
    check(loaded.load(path) && loaded.media_dir == "a\"#;b", "escaped quote broke comment parsing");
    check(write_file_atomic(path, "preserved"), "atomic write failed");
    const std::string victim = temp.path + "/victim";
    std::ofstream(victim) << "unchanged";
    fs::create_symlink(victim, path + ".tmp");
    check(write_file_atomic(path, "replacement") && read_file(victim) == "unchanged", "stale temp symlink followed");
    struct stat mode{}; check(stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600, "config file permissions are not private");
    if (geteuid() == 0) {
        check(chown(path.c_str(), 1000, 1000) == 0 && write_file_atomic(path, "owner preserved"), "host-owned config update failed");
        check(stat(path.c_str(), &mode) == 0 && mode.st_uid == 1000, "config owner changed on container save");
    }
    std::vector<std::jthread> writers;
    std::atomic<bool> write_failed{false};
    for (int i = 0; i < 4; ++i) writers.emplace_back([&, i] { if (!write_file_atomic(path, std::string(8192, char('A' + i)))) write_failed = true; });
    writers.clear();
    std::string final = read_file(path);
    check(!write_failed && final.size() == 8192 && final == std::string(8192, final.front()), "concurrent atomic writes interleaved");
}
static void test_organizer() {
    Temp temp;
    fs::create_directory(temp.path + "/a"); fs::create_directory(temp.path + "/b");
    std::string a = temp.path + "/a/2020-05-03_clip.mp4", b = temp.path + "/b/2020-05-03_clip.MP4";
    std::ofstream(a) << "first media"; std::ofstream(b) << "second media";
    check(organize_media_archive(temp.path, false), "organizer failed duplicate destinations");
    std::set<std::string> contents;
    for (const auto& entry : fs::recursive_directory_iterator(temp.path)) if (entry.is_regular_file()) contents.insert(read_file(entry.path()));
    check(contents == std::set<std::string>{"first media", "second media"}, "organizer overwrote a duplicate destination");
    std::error_code error;
    std::string src = temp.path + "/source", dest = temp.path + "/destination";
    std::ofstream(src) << "source"; std::ofstream(dest) << "destination";
    check(!move_without_replace(src, dest, error) && read_file(src) == "source" && read_file(dest) == "destination", "move replaced existing data");
    sqlite3* db = nullptr;
    std::string journal = temp.path + "/journal.db";
    check(sqlite3_open(journal.c_str(), &db) == SQLITE_OK, "journal open failed");
    check(sqlite3_exec(db, "CREATE TABLE moves(id INTEGER PRIMARY KEY, old_path TEXT, new_path TEXT)", nullptr, nullptr, nullptr) == SQLITE_OK, "journal schema failed");
    std::string successful = temp.path + "/restored";
    sqlite3_stmt* insert = nullptr;
    check(sqlite3_prepare_v2(db, "INSERT INTO moves(old_path,new_path) VALUES(?,?)", -1, &insert, nullptr) == SQLITE_OK, "journal prepare failed");
    for (const auto& old : {successful, dest}) {
        sqlite3_bind_text(insert, 1, old.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, src.c_str(), -1, SQLITE_TRANSIENT);
        check(sqlite3_step(insert) == SQLITE_DONE, "journal insert failed"); sqlite3_reset(insert);
    }
    sqlite3_finalize(insert); sqlite3_close(db);
    extern void undo_organize(const std::string&);
    undo_organize(journal);
    check(read_file(successful) == "source" && read_file(dest) == "destination", "undo overwrote conflict or failed restore");
    sqlite3_open(journal.c_str(), &db);
    check(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM moves", -1, &insert, nullptr) == SQLITE_OK, "journal count failed");
    check(sqlite3_step(insert) == SQLITE_ROW && sqlite3_column_int(insert, 0) == 1, "failed undo lost journal entry");
    sqlite3_finalize(insert); sqlite3_close(db);
}
static void test_http() {
    using namespace http_protocol;
    Request request; size_t required;
    auto parse_text = [&](std::string_view raw) { return parse(raw, request, required); };
    check(parse_text("POST /google_photos_setup HTTP/1.1\r\nHost: local\r\nContent-Length: 3\r\n\r\na=1") == Result::complete && request.content_length == 3,
          "final Content-Length header ignored");
    check(parse_text("POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\na=") == Result::incomplete, "partial body dispatched");
    for (const auto& bad : {"Content-Length: -1", "Content-Length: 3junk", "Content-Length: 1\r\ncontent-length: 1", "Transfer-Encoding: chunked", "Host : nope", "Authorization: a\r\nauthorization: b"})
        check(parse_text(std::string("GET / HTTP/1.1\r\n") + bad + "\r\n\r\n") == Result::bad_request, "ambiguous headers accepted");
    check(parse_text("POST / HTTP/1.1\r\nContent-Length: 65537\r\n\r\n") == Result::too_large, "oversized body accepted");
    std::string raw = "GET /api/next_extra?value=a%26b%2Bc HTTP/1.1\r\nx-test: Authorization: fake\r\n\r\nAuthorization: forged";
    check(parse_text(raw) == Result::complete && request.path == "/api/next_extra" && !request.headers.contains("authorization"), "auth header forged outside header field");
    check(query_param(raw, "value") == "a&b+c" && !query_param(raw, "fake"), "query decoding failed");
    check(url_encode("a+b &") == "a%2Bb%20%26", "form encoding failed");
    int sockets[2]; check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
    std::jthread writer([&] {
        send_all(sockets[1], "POST / HTTP/1.1\r\ncontent-LENGTH: 3\r\n\r\na");
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); send_all(sockets[1], "=1");
    });
    check(receive(sockets[0], raw, request, 1000) == Result::complete && raw.ends_with("a=1"), "fragmented request failed");
    writer.join(); close(sockets[0]); close(sockets[1]);
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
    send_all(sockets[1], "GET / HTTP/1.1\r\nHost: local");
    check(receive(sockets[0], raw, request, 30) == Result::timeout, "incomplete header lacked a deadline");
    close(sockets[1]); check(!send_all(sockets[0], "closed"), "closed peer send succeeded"); close(sockets[0]);
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
    int send_buffer = 1024; setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    check(!send_all(sockets[0], std::string(1024 * 1024, 'x'), 30), "slow reader had no write deadline");
    close(sockets[0]); close(sockets[1]);
}
static void test_oauth() {
    OAuthState state;
    auto now = OAuthState::Clock::now();
    std::string first = state.begin("id", "secret", "http://local/callback", now);
    check(first.size() == 64 && !state.consume("forged", now), "invalid OAuth state accepted");
    auto flow = state.consume(first, now);
    check(flow && flow->client_secret == "secret" && !state.consume(first, now), "OAuth state replay accepted");
    std::string expired = state.begin("id", "secret", "redirect", now);
    check(!state.consume(expired, now + std::chrono::minutes(11)), "expired OAuth state accepted");
}
// One-shot loopback fixture exercises real libcurl status handling and partial bodies.
static std::string serve_once(std::string response, std::jthread& server) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); check(fd >= 0, "fixture socket failed");
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(fd, 1) == 0, "fixture listen failed");
    socklen_t len = sizeof(addr); getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    server = std::jthread([fd, response = std::move(response)] {
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 3000) > 0) {
            int client = accept(fd, nullptr, nullptr);
            if (client >= 0) { char request[1024]; recv(client, request, sizeof(request), 0); http_protocol::send_all(client, response); close(client); }
        }
        close(fd);
    });
    return "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
}
static void test_transfers() {
    check(http_transfer::is_google_media_url("https://lh3.googleusercontent.com/image"), "valid media host rejected");
    check(!http_transfer::is_google_media_url("https://googleusercontent.com:password@evil.example/image"), "userinfo URL bypassed host validation");
    check(!http_transfer::is_google_media_url("https://notgoogleusercontent.com/image"), "host suffix without dot accepted");
    Temp temp; std::jthread server;
    std::string path = temp.path + "/cloud.jpg", text;
    std::string data(2048, 'p');
    auto url = serve_once("HTTP/1.1 500 Error\r\nContent-Length: 2048\r\n\r\n" + data, server);
    check(!http_transfer::download(url, path, 8192) && !fs::exists(path), "HTTP error body published as media"); server.join();
    url = serve_once("HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n" + data, server);
    check(!http_transfer::download(url, path, 8192) && !fs::exists(path), "truncated download published"); server.join();
    url = serve_once("HTTP/1.1 200 OK\r\nContent-Length: 2048\r\n\r\n" + data, server);
    check(!http_transfer::download(url, path, 1024) && !fs::exists(path), "download exceeded disk budget"); server.join();
    url = serve_once("HTTP/1.1 200 OK\r\nContent-Length: 2048\r\n\r\n" + data, server);
    check(http_transfer::download(url, path, 8192) && read_file(path) == data, "complete cloud download failed"); server.join();
    url = serve_once("HTTP/1.1 200 OK\r\nContent-Length: 2048\r\n\r\n" + data, server);
    check(!http_transfer::get(url, text, "", 1024) && text.empty(), "oversized text response retained"); server.join();
    for (const auto& entry : fs::directory_iterator(temp.path)) check(entry.path().extension() == ".jpg", "failed download left a temporary file");
}
static void test_lifecycles() {
    mock_network = true;
    NewsTicker news; GoogleCalendar calendar; StockStreamer stocks;
    auto exercise = [&] {
        for (int i = 0; i < 2; ++i) {
            check(news.start(), "news start failed");
            check(calendar.start(), "calendar start failed");
            check(stocks.start(), "stocks start failed");
            news.stop(); calendar.stop(); stocks.stop();
        }
    };
    std::jthread first(exercise), second(exercise);
    first.join(); second.join();
    news.stop(); calendar.stop(); stocks.stop();
    check(!news.is_running() && !calendar.is_running() && !stocks.is_running(), "worker survived stop");
    mock_network = false;
}

int main() {
    try {
        g_logger.level = LogLevel::ERROR;
        test_config(); test_organizer(); test_http(); test_oauth(); test_transfers(); test_lifecycles();
        std::cout << "Deep config/PIN, organizer, HTTP, OAuth and download regressions passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
