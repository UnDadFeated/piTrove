#include "chores.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <sys/stat.h>
namespace chores {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* p = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &p, nullptr);
    return {p, sqlite3_finalize};
}
std::string column(sqlite3_stmt* s, int n) {
    auto p = sqlite3_column_text(s, n);
    return p ? reinterpret_cast<const char*>(p) : "";
}
bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
Result bad(Code code, const char* message) { return {code, 0, message}; }
// Reject malformed UTF-8 and control characters before passing text to SDL/HTML.
bool text(std::string& value, size_t limit) {
    auto a = value.find_first_not_of(" \t\r\n"), b = value.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    value = value.substr(a, b - a + 1);
    if (value.size() > limit*4) return false;
    size_t characters=0;
    for (size_t i = 0; i < value.size();) {
        if (++characters > limit) return false;
        auto c = static_cast<unsigned char>(value[i++]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        int count; unsigned cp, min;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; cp = c & 31; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; cp = c & 15; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; cp = c & 7; min = 0x10000; }
        else return false;
        if (i + count > value.size()) return false;
        while (count--) { auto d = static_cast<unsigned char>(value[i++]); if ((d & 0xc0) != 0x80) return false; cp = (cp << 6) | (d & 63); }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0x80 && cp <= 0x9f)) return false;
    }
    return true;
}
bool digits(std::string_view s) { return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }
bool color(const std::string& s) {
    return s.size() == 7 && s[0] == '#' && std::all_of(s.begin()+1,s.end(),[](char c) { return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F'); });
}
bool load(sqlite3* db, Snapshot& snapshot, std::set<std::pair<int64_t,std::string>>& completions) {
    auto people = prepare(db, "SELECT id,name,color FROM people ORDER BY id");
    auto tasks = prepare(db, "SELECT id,person_id,title,repeat,date,time FROM chores ORDER BY id");
    auto done = prepare(db, "SELECT chore_id,date FROM completions");
    if (!people || !tasks || !done) return false;
    int rc;
    while ((rc = sqlite3_step(people.get())) == SQLITE_ROW) snapshot.people.push_back({sqlite3_column_int64(people.get(),0), column(people.get(),1), column(people.get(),2)});
    if (rc != SQLITE_DONE) return false;
    while ((rc = sqlite3_step(tasks.get())) == SQLITE_ROW) snapshot.chores.push_back({sqlite3_column_int64(tasks.get(),0), sqlite3_column_int64(tasks.get(),1), column(tasks.get(),2), column(tasks.get(),3), column(tasks.get(),4), column(tasks.get(),5)});
    if (rc != SQLITE_DONE) return false;
    while ((rc = sqlite3_step(done.get())) == SQLITE_ROW) completions.emplace(sqlite3_column_int64(done.get(),0), column(done.get(),1));
    return rc == SQLITE_DONE;
}
}
bool valid_date(const std::string& s) {
    if (s.size()!=10 || s[4]!='-' || s[7]!='-' || !digits(std::string_view(s).substr(0,4)) || !digits(std::string_view(s).substr(5,2)) || !digits(std::string_view(s).substr(8,2))) return false;
    int y = std::stoi(s.substr(0,4)), m = std::stoi(s.substr(5,2)), d = std::stoi(s.substr(8,2));
    return y >= 1900 && std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{unsigned(m)}, std::chrono::day{unsigned(d)}}.ok();
}
bool valid_time(const std::string& s) {
    return s.size()==5 && s[2]==':' && digits(std::string_view(s).substr(0,2)) && digits(std::string_view(s).substr(3,2)) && s.substr(0,2)<"24" && s.substr(3,2)<"60";
}
bool due_today_or_overdue(const Chore& c, const std::string& today) { return valid_date(today) && (c.repeat == "daily" || c.date == today || (c.date < today && !c.completed)); }
Store::~Store() { if (db) sqlite3_close_v2(db); }
bool Store::ready() const { return available.load(std::memory_order_acquire); }
Result Store::open(const std::string& path) {
    std::lock_guard lock(mutex);
    sqlite3* candidate = nullptr;
    if (sqlite3_open_v2(path.c_str(), &candidate, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (candidate) sqlite3_close_v2(candidate);
        return bad(Code::unavailable, "Cannot open chore database");
    }
    auto guard = std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>(candidate,sqlite3_close_v2);
    if (path != ":memory:") chmod(path.c_str(),0600);
    sqlite3_busy_timeout(candidate,2000);
    if (!exec(candidate,"PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;") ||
        !exec(candidate,"BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS people(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,color TEXT NOT NULL); CREATE TABLE IF NOT EXISTS chores(id INTEGER PRIMARY KEY AUTOINCREMENT,person_id INTEGER NOT NULL REFERENCES people(id) ON DELETE RESTRICT,title TEXT NOT NULL,repeat TEXT NOT NULL CHECK(repeat IN ('daily','once')),date TEXT NOT NULL,time TEXT NOT NULL); CREATE TABLE IF NOT EXISTS completions(chore_id INTEGER NOT NULL REFERENCES chores(id) ON DELETE CASCADE,date TEXT NOT NULL,PRIMARY KEY(chore_id,date)); CREATE TRIGGER IF NOT EXISTS chores_schedule_changed AFTER UPDATE OF repeat,date ON chores WHEN old.repeat<>new.repeat OR old.date<>new.date BEGIN DELETE FROM completions WHERE chore_id=new.id; END; CREATE TRIGGER IF NOT EXISTS completion_retention AFTER INSERT ON completions BEGIN DELETE FROM completions WHERE chore_id=new.chore_id AND date NOT IN (SELECT date FROM completions WHERE chore_id=new.chore_id ORDER BY date DESC LIMIT 32); END; COMMIT;")) return bad(Code::unavailable,"Cannot initialize chore database");
    Snapshot next; std::set<std::pair<int64_t,std::string>> done;
    if (!load(candidate,next,done)) return bad(Code::unavailable,"Cannot read chore database");
    if (db) sqlite3_close_v2(db);
    db=guard.release();
    { std::lock_guard cache_lock(cached_mutex); cached=std::move(next); completions=std::move(done); }
    available.store(true,std::memory_order_release);
    return {};
}
Snapshot Store::snapshot(const std::string& today) const {
    std::lock_guard lock(cached_mutex);
    Snapshot result=cached;
    for (auto& c:result.chores) c.completed=completions.contains({c.id,c.repeat=="daily" ? today : c.date});
    return result;
}
// Called with mutex held. Publish the new cache only after a checked commit.
Result Store::mutate(const char* sql, const std::vector<std::string>& fields, int64_t id) {
    if (!db || !exec(db,"BEGIN IMMEDIATE")) return bad(Code::unavailable,"Chore storage is unavailable; try again");
    struct Rollback { sqlite3* db; bool committed=false; ~Rollback() { if (!committed) exec(db,"ROLLBACK"); } } rollback{db};
    auto stmt=prepare(db,sql);
    if (!stmt) return bad(Code::unavailable,"Cannot prepare chore update");
    for (size_t i=0;i<fields.size();++i) if (sqlite3_bind_text(stmt.get(),int(i+1),fields[i].data(),int(fields[i].size()),SQLITE_TRANSIENT)!=SQLITE_OK) return bad(Code::unavailable,"Cannot bind chore update");
    if (sqlite3_step(stmt.get())!=SQLITE_DONE) return bad(Code::unavailable,"Cannot save chore update");
    if (!id) id=sqlite3_last_insert_rowid(db);
    Snapshot next; std::set<std::pair<int64_t,std::string>> done;
    if (!load(db,next,done) || !exec(db,"COMMIT")) return bad(Code::unavailable,"Cannot persist chore update");
    rollback.committed=true;
    { std::lock_guard cache_lock(cached_mutex); cached=std::move(next); completions=std::move(done); }
    return {Code::ok,id,{}};
}
Result Store::save_person(Person p) {
    std::lock_guard lock(mutex);
    if (p.id<0 || !text(p.name,80) || !color(p.color)) return bad(Code::invalid,"Enter a name (up to 80 characters) and a valid color");
    if (p.id && std::none_of(cached.people.begin(),cached.people.end(),[&](auto& v){return v.id==p.id;})) return bad(Code::not_found,"Person no longer exists");
    if (!p.id && cached.people.size()>=64) return bad(Code::conflict,"The household has reached its 64-person limit");
    return p.id ? mutate("UPDATE people SET name=?,color=? WHERE id=?",{p.name,p.color,std::to_string(p.id)},p.id) : mutate("INSERT INTO people(name,color) VALUES(?,?)",{p.name,p.color});
}
Result Store::delete_person(int64_t id) {
    std::lock_guard lock(mutex);
    if (std::none_of(cached.people.begin(),cached.people.end(),[&](auto& v){return v.id==id;})) return bad(Code::not_found,"Person no longer exists");
    if (std::any_of(cached.chores.begin(),cached.chores.end(),[&](auto& v){return v.person_id==id;})) return bad(Code::conflict,"Reassign or delete this person's chores first");
    return mutate("DELETE FROM people WHERE id=?",{std::to_string(id)},id);
}
Result Store::save_chore(Chore c) {
    std::lock_guard lock(mutex);
    if (c.id<0 || !text(c.title,160) || (c.repeat!="daily" && c.repeat!="once") || (c.repeat=="once" && !valid_date(c.date)) || (!c.time.empty() && !valid_time(c.time))) return bad(Code::invalid,"Enter a chore, a valid schedule/date, and an optional time");
    if (c.repeat=="daily") c.date.clear();
    if (std::none_of(cached.people.begin(),cached.people.end(),[&](auto& p){return p.id==c.person_id;})) return bad(Code::invalid,"Choose an existing person for this chore");
    if (c.id && std::none_of(cached.chores.begin(),cached.chores.end(),[&](auto& v){return v.id==c.id;})) return bad(Code::not_found,"Chore no longer exists");
    if (!c.id && cached.chores.size()>=512) return bad(Code::conflict,"The household has reached its 512-chore limit");
    // A changed date/repetition represents a new schedule; drop old occurrence completions atomically.
    const char* update="UPDATE chores SET person_id=?,title=?,repeat=?,date=?,time=? WHERE id=?";
    std::vector<std::string> fields{std::to_string(c.person_id),c.title,c.repeat,c.date,c.time};
    if (c.id) fields.push_back(std::to_string(c.id));
    return mutate(c.id ? update : "INSERT INTO chores(person_id,title,repeat,date,time) VALUES(?,?,?,?,?)",fields,c.id);
}
Result Store::delete_chore(int64_t id) {
    std::lock_guard lock(mutex);
    if (std::none_of(cached.chores.begin(),cached.chores.end(),[&](auto& v){return v.id==id;})) return bad(Code::not_found,"Chore no longer exists");
    return mutate("DELETE FROM chores WHERE id=?",{std::to_string(id)},id);
}
Result Store::set_completed(int64_t id, const std::string& date, bool completed) {
    std::lock_guard lock(mutex);
    auto it=std::find_if(cached.chores.begin(),cached.chores.end(),[&](auto& c){return c.id==id;});
    if (it==cached.chores.end()) return bad(Code::not_found,"Chore no longer exists");
    if (!valid_date(date) || (it->repeat=="once" && date!=it->date)) return bad(Code::invalid,"Completion date must match the chore occurrence");
    // Keep the latest 32 completed daily occurrences per chore; once-only completion persists.

    return mutate(completed ? "INSERT OR IGNORE INTO completions(chore_id,date) VALUES(?,?)" : "DELETE FROM completions WHERE chore_id=? AND date=?",{std::to_string(id),date},id);
}
}
