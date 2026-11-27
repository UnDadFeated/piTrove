// Minimal process services for isolated tests of production font/scanner code.
#include "util.h"
#include "renderer.h"
#include "config.h"
#include "error_db.h"
#include "image_loader.h"
#include <filesystem>

Logger::~Logger() = default;
void Logger::enqueue(LogMessage) {} // Logging is outside the font/scanner/cache fixture.
std::string redact_secrets(const std::string& text) { return text; }
void trigger_error(int code) { g_active_error_code.store(code); }
void clear_error(int code) { if (g_active_error_code == code) g_active_error_code = 0; }
bool is_error_active(int code) { return g_active_error_code == code; }
uint64_t get_available_ram_bytes() { return 1024ULL * 1024 * 1024; }
std::string get_exe_dir() { return std::filesystem::current_path().string(); }
Renderer::Renderer() = default;
Renderer::~Renderer() = default;

Config::Config() = default;

bool is_nas_online() { return true; }
std::vector<uint8_t> ImageLoader::read_file_to_buffer(const std::string&) { return {}; }
ImageMetadata ImageLoader::read_metadata_from_memory(std::span<const uint8_t>) { return {}; }

bool set_display_power(bool power) { return g_screen_blanked.exchange(!power) != !power; }

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}
