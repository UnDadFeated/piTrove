#ifndef PITROVE_AUTH_H
#define PITROVE_AUTH_H

#include <string>

namespace pitrove { namespace auth {

bool init();
bool hash_pin(const std::string& pin, std::string& out);
bool verify_configured_pin(const std::string& input, const std::string& plaintext, const std::string& hash);
bool verify_pin(const std::string& pin, const std::string& hash);

}} // namespace pitrove::auth

#endif // PITROVE_AUTH_H
