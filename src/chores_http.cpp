#include "chores_http.h"
#include "http_protocol.h"
#include <charconv>
#include <sstream>
namespace chores {
namespace {
std::string quote(const std::string& s) {
    std::string out="\""; constexpr char hex[]="0123456789abcdef";
    for (unsigned char c:s) {
        if (c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if (c<0x20) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=char(c);
    }
    return out+'"';
}
HttpResponse error(int status,const std::string& message) { return {status,"{\"error\":"+quote(message)+"}"}; }
HttpResponse result(const Result& r) {
    if (r.ok()) return {200,"{\"id\":"+std::to_string(r.id)+",\"status\":\"ok\"}"};
    int status=r.code==Code::invalid?400:r.code==Code::not_found?404:r.code==Code::conflict?409:503;
    return error(status,r.error);
}
}
HttpResponse handle_http(Store& store,const std::string& method,const std::string& path,const std::string& raw,const std::string& today,const std::string& timezone) {
    const bool read=path=="/api/chores";
    const bool write=path=="/api/chores/people/save" || path=="/api/chores/people/delete" || path=="/api/chores/tasks/save" || path=="/api/chores/tasks/delete" || path=="/api/chores/tasks/complete";
    if (!read && !write) return error(404,"Unknown chores endpoint");
    if ((read && method!="GET") || (write && method!="POST")) return error(405,"Use GET to read chores and POST to change them");
    if (!store.ready()) return error(503,"Chore storage is unavailable");
    if (!valid_date(today)) return error(503,"Cannot determine the household's local date");
    if (read) {
        const auto state=store.snapshot(today); std::ostringstream out;
        out<<"{\"today\":"<<quote(today)<<",\"timezone\":"<<quote(timezone)<<",\"people\":[";
        bool first=true;
        for (const auto& p:state.people) { if (!first) out<<','; first=false; out<<"{\"id\":"<<p.id<<",\"name\":"<<quote(p.name)<<",\"color\":"<<quote(p.color)<<'}'; }
        out<<"],\"chores\":["; first=true;
        for (const auto& c:state.chores) {
            if (!first) out<<',';
            first=false;
            out<<"{\"id\":"<<c.id<<",\"person_id\":"<<c.person_id<<",\"title\":"<<quote(c.title)<<",\"repeat\":"<<quote(c.repeat)<<",\"date\":"<<quote(c.date)<<",\"time\":"<<quote(c.time)<<",\"all_day\":"<<(c.time.empty()?"true":"false")<<",\"completed\":"<<(c.completed?"true":"false")<<",\"due\":"<<(due_today_or_overdue(c,today)?"true":"false")<<'}';
        }
        out<<"]}"; return {200,out.str()};
    }
    http_protocol::Request parsed; size_t required;
    if (http_protocol::parse(raw,parsed,required)!=http_protocol::Result::complete) return error(400,"Incomplete chore request");
    if (auto origin=parsed.headers.find("origin"); origin!=parsed.headers.end()) {
        const auto host=parsed.headers.find("host");
        if (host==parsed.headers.end() || (origin->second!="http://"+host->second && origin->second!="https://"+host->second))
            return error(403,"Open chores from your piTrove controller to make changes");
    }
    const auto type=parsed.headers.find("content-type");
    if (type==parsed.headers.end() || type->second.substr(0,type->second.find(';'))!="application/x-www-form-urlencoded") return error(415,"Send form-encoded chore fields");
    // Mutations read form bodies only. Query strings cannot override a posted ID/assignee.
    const auto body_start=raw.find("\r\n\r\n")+4;
    const std::string body=raw.substr(body_start,parsed.content_length);
    const std::string form="POST / HTTP/1.1\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    auto field=[&](const char* name){return http_protocol::query_param(form,name).value_or("");};
    auto id=[&](const char* name,bool optional=false) {
        const auto s=field(name); int64_t value=0;
        if (s.empty() && optional) return int64_t(0);
        auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),value);
        return ec==std::errc{} && end==s.data()+s.size() && value>0 ? value : int64_t(-1);
    };
    if (path=="/api/chores/people/save") return result(store.save_person({id("id",true),field("name"),field("color")}));
    if (path=="/api/chores/people/delete") return result(store.delete_person(id("id")));
    if (path=="/api/chores/tasks/delete") return result(store.delete_chore(id("id")));
    if (path=="/api/chores/tasks/complete") {
        const auto value=field("completed"); if (value!="1" && value!="0") return error(400,"Choose a valid completion state");
        const auto date=field("date");
        if (!valid_date(date)) return error(400,"Choose a valid occurrence date");
        const auto state=store.snapshot(today);
        for (const auto& c:state.chores) if (c.id==id("id") && c.repeat=="daily" && date!=today)
            return error(409,"A new local day has started. Refresh the board and try again.");
        return result(store.set_completed(id("id"),date,value=="1"));
    }
    const auto all_day=field("all_day");
    if (all_day!="1" && all_day!="0") return error(400,"Choose all day or a time");
    if (all_day=="0" && !valid_time(field("time"))) return error(400,"Choose a valid chore time");
    return result(store.save_chore({id("id",true),id("person_id"),field("title"),field("repeat"),field("date"),all_day=="1" ? "" : field("time")}));
}
}
