#include "safe_mode.h"
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace pitrove { namespace safe_mode {

struct CrashState {
    int32_t count = 0;
    int64_t window_start = 0;
};

static const std::filesystem::path state_path =
    "/app/cache/run/crash_state.bin";

static CrashState read_state() {
    CrashState state;
    std::ifstream in(state_path, std::ios::binary);
    if (in) {
        in.read(reinterpret_cast<char*>(&state), sizeof(state));
    }
    return state;
}

static void write_state(const CrashState& state) {
    std::error_code ec;
    std::filesystem::create_directories(state_path.parent_path(), ec);

    std::ofstream out(state_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&state), sizeof(state));
}

void record_crash() {
    CrashState state = read_state();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    if (state.window_start == 0 || now - state.window_start > 300) {
        state.count = 0;
        state.window_start = now;
    }

    ++state.count;
    write_state(state);
}

bool should_enter_safe_mode() {
    CrashState state = read_state();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    if (state.window_start != 0 && now - state.window_start <= 300) {
        return state.count >= 3;
    }

    return false;
}

void clear() {
    std::error_code ec;
    std::filesystem::remove(state_path, ec);
}

}} // namespace pitrove::safe_mode
