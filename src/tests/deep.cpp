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
#include "google_photos.h"
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
extern "C" CURLMcode __real_curl_multi_perform(CURLM*, int*);
extern "C" CURLMcode __wrap_curl_multi_perform(CURLM* multi, int* running) {
    if (mock_network) {
        *running = 0;
        return CURLM_OK;
    }
    return __real_curl_multi_perform(multi, running);
}
extern "C" CURLcode __real_curl_easy_perform(CURL*);
extern "C" CURLcode __wrap_curl_easy_perform(CURL* curl) {
    if (mock_network) return CURLE_ABORTED_BY_CALLBACK;
    char* url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
    // Exercise transfer failures using the fixture's cleartext loopback server.
    // Redirects still use the production HTTPS and redirect-limit settings.
    if (url && std::string_view(url).starts_with("http://127.0.0.1:")) {
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, nullptr);
    }
    return __real_curl_easy_perform(curl);
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
static void test_logger() {
    Logger logger;
    logger.level = LogLevel::ERROR;
    for (size_t i = 0; i < Logger::MAX_QUEUED_MESSAGES + 20; ++i) logger.error("small message");
    check(logger.front_queue.size() == Logger::MAX_QUEUED_MESSAGES && logger.dropped_messages == 20, "logger message budget failed");
    logger.front_queue.clear(); logger.queued_bytes = 0; logger.dropped_messages = 0;
    std::vector<std::jthread> writers;
    for (int i = 0; i < 4; ++i) writers.emplace_back([&] { for (int n = 0; n < 40; ++n) logger.error("{}", std::string(Logger::MAX_MESSAGE_BYTES * 2, 'x')); });
    writers.clear();
    check(logger.queued_bytes <= Logger::MAX_QUEUED_BYTES && logger.dropped_messages > 0, "logger byte budget failed under concurrent producers");
    for (const auto& message : logger.front_queue) check(message.body.size() <= Logger::MAX_MESSAGE_BYTES, "logger message truncation failed");
    logger.front_queue.clear(); logger.queued_bytes = 0; logger.flush_running = false;
    int pipes[2]; check(pipe(pipes) == 0, "fallback capture failed");
    int previous = dup(STDERR_FILENO); check(previous >= 0 && dup2(pipes[1], STDERR_FILENO) >= 0, "fallback redirect failed");
    logger.error("fallback diagnostic"); fflush(stderr);
    dup2(previous, STDERR_FILENO); close(previous); close(pipes[1]);
    char captured[256]{}; ssize_t bytes = read(pipes[0], captured, sizeof(captured)); close(pipes[0]);
    check(bytes > 0 && std::string_view(captured, size_t(bytes)).find("fallback diagnostic") != std::string_view::npos && logger.front_queue.empty(), "failed logger worker queued or lost diagnostics");
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
    config.left_panel_enabled = true; config.left_panel_max_chores = 17; config.left_panel_show_completed = true;
    config.mqtt_tls_enabled = true; config.mqtt_ca_cert = text;
    config.dashboard_pin = "4391"; config.web_dashboard_enabled = true;
    config.video_decode_budget_enabled = true;
    config.auto_display_rotation = true;
    config.brightness_auto = true;
    config.brightness_auto_min = 23;
    config.brightness_auto_max = 89;
    check(config.save(path), "config save failed");
    check(config.dashboard_pin.empty() && !config.pin_hash.empty(), "PIN migration did not update memory");
    check(pitrove::auth::verify_configured_pin("4391", "", config.pin_hash), "saved PIN did not verify");
    check(!pitrove::auth::verify_configured_pin("wrong", "", config.pin_hash), "hash PIN bypassed");
    check(pitrove::auth::verify_configured_pin("anything", "", ""), "no-PIN mode changed");
    std::string saved = read_file(path);
    check(saved.find("dashboard_pin = \"4391\"") == saved.npos, "plaintext PIN persisted");
    Config loaded;
    check(loaded.load(path), "config reload failed");
    check(loaded.left_panel_enabled && loaded.left_panel_max_chores == 17 && loaded.left_panel_show_completed, "left panel settings dropped on save/load");
    check(loaded.media_dir == text, "media path changed on reload");
    check(loaded.video_decode_budget_enabled && loaded.auto_display_rotation &&
          loaded.brightness_auto && loaded.brightness_auto_min == 23 &&
          loaded.brightness_auto_max == 89, "saved display or decode settings dropped on reload");
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
    for (const auto& entry : fs::recursive_directory_iterator(temp.path)) if (entry.is_regular_file() && !entry.path().filename().string().starts_with('.')) contents.insert(read_file(entry.path()));
    check(contents == std::set<std::string>{"first media", "second media"}, "organizer overwrote a duplicate destination");
    check(undo_organize(temp.path + "/.pitrove-organizer.sqlite3"), "undo organizer move");
    check(read_file(a) == "first media" && read_file(b) == "second media", "organizer did not persist a usable undo journal");
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
    check(!undo_organize(journal), "undo did not report the retained conflicting move");
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
    Request browser;
    check(http_protocol::controller_host_allowed("PiTrove.local:9000"), "mDNS host rejected");
    check(http_protocol::controller_host_allowed("192.168.1.2:9000"), "LAN IP rejected");
    check(!http_protocol::controller_host_allowed("localhost:65536"), "invalid port accepted");
    check(!http_protocol::controller_host_allowed("outside.example"), "untrusted domain accepted");
    browser.headers = {{"host", "frame.test:9000"}, {"origin", "http://frame.test:9000"},
                       {"referer", "http://frame.test:9000/chores"}, {"sec-fetch-site", "same-origin"}};
    check(controller_origin_allowed(browser), "same-origin controller request rejected");
    browser.headers["sec-fetch-site"] = "same-site";
    check(!controller_origin_allowed(browser), "different origin allowed by same-site metadata");
    browser.headers.erase("sec-fetch-site");
    browser.headers.erase("origin");
    browser.headers["referer"] = "http://different.test/";
    check(!controller_origin_allowed(browser), "foreign Referrer accepted");
    browser.headers.clear();
    check(controller_origin_allowed(browser), "command-line request without browser headers rejected");
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
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair failed");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    check(send_until(sockets[0], "header", deadline), "initial response chunk failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(!send_until(sockets[0], "body", deadline), "response chunk reset its absolute deadline");
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
    url = serve_once("HTTP/1.1 400 Error\r\nContent-Length: 23\r\n\r\n{\"error\":\"bad_request\"}", server);
    check(!http_transfer::post_form(url, "code=test", text) && text == "{\"error\":\"bad_request\"}", "OAuth error status or diagnostic body lost"); server.join();
    std::jthread redirect_server;
    auto redirected = serve_once("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", redirect_server);
    url = serve_once("HTTP/1.1 307 Redirect\r\nLocation: " + redirected + "\r\nContent-Length: 0\r\n\r\n", server);
    check(!http_transfer::post_form(url, "client_secret=fixture", text), "credential form followed a redirect"); server.join(); redirect_server.join();
    for (const auto& entry : fs::directory_iterator(temp.path)) check(entry.path().extension() == ".jpg", "failed download left a temporary file");
}
static void test_calendar_recurrence() {
    const auto now = time(nullptr);
    const auto date = [&](int offset) {
        using namespace std::chrono;
        return std::format("{:%Y%m%d}", year_month_day(floor<days>(system_clock::from_time_t(now)) + days(offset)));
    };
    const auto one = date(1), two = date(2), three = date(3), four = date(4);
    const std::string text = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:fixture\r\n"
        "DTSTART;TZID=America/New_York:" + one + "T140000\r\n"
        "DTEND;TZID=America/New_York:" + one + "T153000\r\n"
        "RRULE:FREQ=DAILY;COUNT=3\r\n"
        "EXDATE;TZID=America/New_York:" + two + "T140000\r\n"
        "SUMMARY:Weekly\\, check-in\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:other\r\nDTSTART;VALUE=DATE:" + three + "\r\nDTEND;VALUE=DATE:" + four +
        "\r\nSUMMARY:All day\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    std::jthread server;
    const auto url = serve_once("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(text.size()) + "\r\n\r\n" + text, server);
    Config saved;
    {
        std::unique_lock lock(g_config_mtx);
        saved = g_cfg;
        g_cfg.gcalendar_ical_url = url;
        g_cfg.timezone = "America/New_York";
        g_cfg.gcalendar_max_events = 10;
    }
    GoogleCalendar calendar;
    calendar.sync();
    server.join();
    {
        std::unique_lock lock(g_config_mtx);
        g_cfg = saved;
    }
    const auto events = calendar.get_events();
    check(events.size() == 3, "calendar recurrence or EXDATE failed");
    check(events[0].summary == "Weekly, check-in" && events[0].formatted_time == "02:00 PM - 03:30 PM",
          "calendar TZID, DTEND, or text decoding failed");
    check(events[1].all_day && events[1].formatted_time == "ALL DAY" &&
          events[2].summary == events[0].summary, "calendar all-day ordering failed");
}

static void test_lifecycles() {
    mock_network = true;
    NewsTicker news; GoogleCalendar calendar; StockStreamer stocks; GooglePhotosManager photos;
    { std::unique_lock lock(g_config_mtx); g_cfg.google_photos_enabled = true; }
    auto exercise = [&] {
        for (int i = 0; i < 2; ++i) {
            check(news.start(), "news start failed");
            check(calendar.start(), "calendar start failed");
            check(stocks.start(), "stocks start failed");
            photos.start();
            news.stop(); calendar.stop(); stocks.stop(); photos.stop();
        }
    };
    std::jthread first(exercise), second(exercise);
    first.join(); second.join();
    news.stop(); calendar.stop(); stocks.stop(); photos.stop();
    check(!news.is_running() && !calendar.is_running() && !stocks.is_running(), "worker survived stop");
    // Every quote request in this fixture failed. Never publish sample prices as market data.
    check(stocks.get_stocks().empty() && stocks.get_crypto().last_updated_epoch == 0,
          "missing market feed produced fabricated quotes");
    const auto status = stocks.get_status_json();
    check(status.find("\"last_sync\":0") != std::string::npos &&
          status.find("\"available\":false") != std::string::npos,
          "failed market feed reported a successful refresh");

    check(!photos.is_running(), "Google Photos worker survived stop");
    { std::unique_lock lock(g_config_mtx); g_cfg.google_photos_enabled = false; }
    check(news.get_status_json().find("\"last_sync\":0") != std::string::npos,
          "stopped news feeds reported a successful refresh");
    // Run one synchronous failed refresh so the error-reporting check does not race stop().
    NewsTicker failed_news;
    failed_news.sync();
    const auto news_status = failed_news.get_status_json();
    check(news_status.find("\"last_sync\":0") != std::string::npos &&
          news_status.find("\"last_error\":537") != std::string::npos,
          "failed news feeds reported a successful refresh");
    mock_network = false;
}

int main() {
    try {
        g_logger.level = LogLevel::ERROR;
        check(redact_secrets("pass = mysecret123").find("mysecret123") == std::string::npos, "pass secret leaked");
    check(redact_secrets("ical_url=\"https://cal.example/private token\"").find("private") == std::string::npos, "ical_url secret leaked");
    test_logger(); test_config(); test_organizer(); test_http(); test_oauth(); test_transfers(); test_calendar_recurrence(); test_lifecycles();
        std::cout << "Deep config/PIN, organizer, HTTP, OAuth and download regressions passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
