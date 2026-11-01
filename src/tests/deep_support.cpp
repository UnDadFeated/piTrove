// Only metadata/database services outside these isolated tests are substituted.
#include "image_loader.h"
#include "cache.h"
int64_t ImageLoader::get_creation_time(std::string_view) { return 0; }
bool CacheManager::get_error_details(const std::string&, std::string&, std::string&, std::string&) { return false; }
