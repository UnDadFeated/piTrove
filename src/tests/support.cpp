// Minimal process services for isolated tests of production font/scanner code.
#include "util.h"
#include "renderer.h"
#include "config.h"
#include <filesystem>

Logger::~Logger() = default;
std::string redact_secrets(const std::string& text) { return text; }
void trigger_error(int code) { g_active_error_code.store(code); }
void clear_error(int code) { if (g_active_error_code == code) g_active_error_code = 0; }
bool is_error_active(int code) { return g_active_error_code == code; }
uint64_t get_available_ram_bytes() { return 1024ULL * 1024 * 1024; }
std::string get_exe_dir() { return std::filesystem::current_path().string(); }
Renderer::Renderer() = default;
Renderer::~Renderer() = default;

Config::Config() = default;
