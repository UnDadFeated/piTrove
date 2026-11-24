#include "auth.h"
#include <sodium.h>

namespace pitrove { namespace auth {

bool init() {
    return sodium_init() >= 0;
}

bool hash_pin(const std::string& pin, std::string& out) {
    if (!init()) return false;
    char buf[crypto_pwhash_STRBYTES];

    if (crypto_pwhash_str(buf,
                          pin.c_str(),
                          static_cast<unsigned long long>(pin.size()),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        return false;
    }

    out = buf;
    return true;
}

bool verify_pin(const std::string& pin, const std::string& hash) {
    if (!init()) return false;
    return crypto_pwhash_str_verify(hash.c_str(),
                                    pin.c_str(),
                                    static_cast<unsigned long long>(pin.size())) == 0;
}

bool verify_configured_pin(const std::string& input, const std::string& plaintext, const std::string& hash) {
    if (!hash.empty()) return verify_pin(input, hash);
    if (plaintext.empty()) return true; // Preserve the deliberate no-PIN mode.
    return input.size() == plaintext.size() && sodium_memcmp(input.data(), plaintext.data(), input.size()) == 0;
}

}} // namespace pitrove::auth
