#ifndef PITROVE_ORGANIZER_H
#define PITROVE_ORGANIZER_H

#include <string>
#include <vector>

// Reorganize the media library archive using C++17 <filesystem> operations.
// Returns true on success, false on failure.
bool organize_media_archive(const std::string& root_dir, bool in_place,
                            const std::vector<std::string>& ignore_folders = {});
bool undo_organize(const std::string& journal_path);

#endif // PITROVE_ORGANIZER_H
