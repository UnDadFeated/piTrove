#pragma once
#include "config.h"
#include <map>
#include <string>
#include <vector>
namespace config_editor {
struct FieldView {
    std::string id, label, group, help, type, value;
    std::vector<std::string> choices;
    bool secret, configured;
};
std::vector<FieldView> fields(const Config& config);
struct Result { int status; std::string body; std::vector<std::string> changed; };
// Call with the config lock held (exclusive for apply). Pure model + atomic persistence.
std::string describe(const Config& config);
std::string revision(const Config& config);
Result apply(Config& config, const std::map<std::string,std::string>& fields, const std::string& path);
Result handle(Config& config, const std::string& method, const std::string& raw, const std::string& path);
}
