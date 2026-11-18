#include "util.h"
#include <iostream>
#include <stdexcept>

// Initialized after the logger, then destroyed after main's local/static work.
// The previous lazy redaction regexes were destroyed before this cleanup log.
struct LateCleanup {
    ~LateCleanup() { g_logger.info("Final cleanup token=fixture-secret"); }
};
static LateCleanup cleanup;
int main() {
    const auto text=redact_secrets("Bearer fixture-key password=fixture-secret");
    if(text.find("fixture-key")!=std::string::npos||text.find("fixture-secret")!=std::string::npos)
        throw std::runtime_error("Logger did not redact secrets");
    g_logger.info("Normal shutdown begins");
    std::cout<<"Logger redaction and late static cleanup regression\n";
    return 0;
}
