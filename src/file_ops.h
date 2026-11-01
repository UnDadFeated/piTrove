#pragma once
#include <filesystem>
#include <system_error>

// Atomic no-replace move on Linux. Unsupported filesystems fail without replacing data.
bool move_without_replace(const std::filesystem::path& source,
                          const std::filesystem::path& destination, std::error_code& error);
// Atomic replacement for configuration files, using a unique, private temporary file.
bool write_file_atomic(const std::string& path, const std::string& contents);
