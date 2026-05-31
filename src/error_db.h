#ifndef PITROVE_ERROR_DB_H
#define PITROVE_ERROR_DB_H

#include <string>
#include <vector>

struct ErrorSeed {
    std::string code;
    std::string title;
    std::string desc;
    std::string rec;
};

// Returns a complete vector of all diagnostic error catalog definitions
std::vector<ErrorSeed> get_all_error_seeds();

#endif // PITROVE_ERROR_DB_H
