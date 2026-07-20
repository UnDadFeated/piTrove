#include "auth.h"
#include <sodium.h>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace pitrove { namespace auth {

bool init() {
    return sodium_init() >= 0;
}

bool hash_pin(const std::string& pin, std::string& out) {
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
    return crypto_pwhash_str_verify(hash.c_str(),
                                    pin.c_str(),
                                    static_cast<unsigned long long>(pin.size())) == 0;
}

struct Attempt {
    int count = 0;
    std::time_t window_start = 0;
};

static std::mutex rate_mutex;
static std::unordered_map<std::string, Attempt> attempts;

bool pin_rate_limit_allowed(const std::string& ip,
                            int max_attempts,
                            int window_seconds) {
    std::lock_guard<std::mutex> lock(rate_mutex);

    const std::time_t now = std::time(nullptr);
    Attempt& a = attempts[ip];

    if (a.window_start == 0 || now - a.window_start >= window_seconds) {
        a.count = 0;
        a.window_start = now;
    }

    if (a.count >= max_attempts) {
        return false;
    }

    ++a.count;
    return true;
}

}} // namespace pitrove::auth
