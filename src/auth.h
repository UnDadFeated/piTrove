#ifndef PITROVE_AUTH_H
#define PITROVE_AUTH_H

#include <string>

namespace pitrove { namespace auth {

bool init();
bool hash_pin(const std::string& pin, std::string& out);
bool verify_pin(const std::string& pin, const std::string& hash);
bool pin_rate_limit_allowed(const std::string& ip,
                            int max_attempts = 5,
                            int window_seconds = 60);

}} // namespace pitrove::auth

#endif // PITROVE_AUTH_H
