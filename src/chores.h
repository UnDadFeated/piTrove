#pragma once
#include <cstdint>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>
struct sqlite3;
namespace chores {
struct Person { int64_t id = 0; std::string name, color = "#38bdf8"; };
struct Chore {
    int64_t id = 0, person_id = 0;
    std::string title, repeat = "daily", date, time;
    bool completed = false;
};
struct Snapshot { std::vector<Person> people; std::vector<Chore> chores; };
enum class Code { ok, invalid, not_found, conflict, unavailable };
struct Result {
    Code code = Code::ok; int64_t id = 0; std::string error;
    bool ok() const { return code == Code::ok; }
};
bool valid_date(const std::string& date);
bool valid_time(const std::string& time);
bool due_today_or_overdue(const Chore& chore, const std::string& today);
class Store {
public:
    ~Store();
    Result open(const std::string& path);
    bool ready() const;
    Snapshot snapshot(const std::string& today) const;
    Result save_person(Person person);
    Result delete_person(int64_t id);
    Result save_chore(Chore chore);
    Result delete_chore(int64_t id);
    Result set_completed(int64_t id, const std::string& date, bool completed, std::string_view today = {});
private:
    mutable std::mutex mutex, cached_mutex;
    std::atomic<bool> available{false};
    sqlite3* db = nullptr;
    Snapshot cached;
    std::set<std::pair<int64_t, std::string>> completions;
    Result mutate(const char* sql, const std::vector<std::string>& fields, int64_t id = 0);
};
inline Store g_store;
}
