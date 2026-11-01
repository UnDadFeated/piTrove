#pragma once
#include <sodium.h>
#include <string>
#include <optional>
#include <mutex>
#include <chrono>

class OAuthState {
public:
    using Clock = std::chrono::steady_clock;
    struct Flow { std::string state, client_id, client_secret, redirect_uri; Clock::time_point expires; };
    std::string begin(std::string id, std::string secret, std::string redirect, Clock::time_point now = Clock::now()) {
        if (sodium_init() < 0) return "";
        unsigned char random[32]; randombytes_buf(random, sizeof(random));
        char hex[65]; sodium_bin2hex(hex, sizeof(hex), random, sizeof(random));
        std::lock_guard lock(mutex);
        pending = Flow{hex, std::move(id), std::move(secret), std::move(redirect), now + std::chrono::minutes(10)};
        return pending->state;
    }
    std::optional<Flow> consume(const std::string& state, Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mutex);
        if (!pending) return std::nullopt;
        if (now >= pending->expires) { pending.reset(); return std::nullopt; }
        if (state.size() != pending->state.size() || sodium_memcmp(state.data(), pending->state.data(), state.size()) != 0) return std::nullopt;
        auto result = std::move(pending); pending.reset(); return result;
    }
private:
    std::mutex mutex;
    std::optional<Flow> pending;
};
