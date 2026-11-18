#include "chores.h"
#include "chores_http.h"
#include "infopanel_layout.h"
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>
using namespace chores;
static void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
static std::string request(std::string form,std::string query="") {return "POST /api/chores/tasks/save"+query+" HTTP/1.1\r\nHost: local\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: "+std::to_string(form.size())+"\r\n\r\n"+form;}
static void test_geometry(){
 for(auto [w,h]:{std::pair{1920,1080},std::pair{3840,2160},std::pair{1280,720},std::pair{2560,1440},std::pair{800,480}})
 for(bool matte:{false,true})for(bool news:{false,true})for(bool left:{false,true})for(bool calendar:{false,true})for(bool stocks:{false,true}) {
    InfopanelOptions o{true,news,calendar,stocks,left,matte,96,17,-11};auto l=make_infopanel_layout(w,h,o);
    check(l.slideshow_area.w>0 && l.slideshow_area.h>0,"invalid slideshow dimensions");
    if(left)check(l.chores_area.x+l.chores_area.w<=l.slideshow_area.x,"left panel overlaps slideshow");
    const int caption=filename_overlay_x(w,o.offset_x,left,l.slideshow_area.x);
    const int expected=left ? l.chores_area.x+l.chores_area.w+int(std::round(12.0*w/1920.0)) : int(std::round(72.0*w/1920.0))+o.offset_x;
    check(caption==expected,"filename padding applied physical matte twice or lost display offset");
    SDL_Rect right=calendar?l.calendar_area:l.stocks_area;
    if(calendar||stocks)check(l.slideshow_area.x+l.slideshow_area.w<=right.x,"right panel overlaps slideshow");
    if(left&&(calendar||stocks)){
        check(l.chores_area.w==right.w,"side panels not equal width");
        check(std::abs((l.slideshow_area.x*2+l.slideshow_area.w)-(w+34))<=1,"slideshow not centered between equal panels");
    }
    if(news){
        for(auto rect:{l.slideshow_area,l.chores_area,l.calendar_area,l.stocks_area})if(rect.w>0)check(rect.y+rect.h<=l.news_area.y,"content overlaps bottom news feed");
    }
    o.enabled=false;l=make_infopanel_layout(w,h,o);check(!l.show_chores&&!l.show_calendar&&!l.show_stocks&&!l.show_news,"master switch ignored");
 }
 InfopanelOptions o{true,true,true,true,true,true,140,0,0};auto l=make_infopanel_layout(1920,1080,o);
 check(l.chores_area.x==60 && l.chores_area.w==275 && l.slideshow_area.x==335 && l.slideshow_area.w==1250 && l.calendar_area.x==1585,"reference matte calibration moved");
 check(filename_overlay_x(1920,0,true,l.slideshow_area.x)==347,"filename must be 12px beyond the 335px chores border");
 check(l.news_area.y>=l.slideshow_area.y+l.slideshow_area.h,"reference newsfeed overlaps content");
 // The editor supports custom aspect ratios and large mattes. Even with very
 // little vertical space, neither right panel can draw into the ticker.
 o.matting_size = 500;
 const auto wide = make_infopanel_layout(7680, 480, o);
 check(wide.content_top==16,"visible opening top did not scale with display height");
 check(wide.calendar_area.y+wide.calendar_area.h<=wide.stocks_area.y &&
       wide.stocks_area.y+wide.stocks_area.h<=wide.news_area.y,
       "wide short display has overlapping side panels or ticker");
 check(wide.news_area.y+wide.news_area.h<=480-16,"ticker extends behind the bottom matte");
}
static void test_store(const std::string& path){
 Store store;check(store.open(path).ok(),"store open failed");
 auto result=store.save_person({0,"  Zoë & <Alex>  ","#aabbcc"});check(result.ok(),"Unicode person rejected");int64_t person=result.id;
 check(store.snapshot("2026-11-01").people[0].name=="Zoë & <Alex>","person whitespace not trimmed");
 check(!store.save_person({0,"bad\nname","#aabbcc"}).ok(),"control characters accepted");
 check(!store.save_person({0,std::string("bad\xc0\xaf"),"#aabbcc"}).ok(),"malformed UTF-8 accepted");
 check(!store.save_person({0,"Good","red"}).ok(),"unsafe color accepted");
 auto daily=store.save_chore({0,person,"Feed cat","daily","","08:30"});check(daily.ok(),"daily chore rejected");
 auto once=store.save_chore({0,person,"Clean windows","once","2026-11-01",""});check(once.ok(),"all-day dated chore rejected");
 check(store.delete_person(person).code==Code::conflict,"assigned person deleted");
 check(!store.save_chore({0,99999,"Orphan","daily","",""}).ok(),"missing assignee accepted");
 check(!store.save_chore({0,person,"Impossible date","once","2026-02-29",""}).ok(),"invalid calendar date accepted");
 check(!store.save_chore({0,person,"Bad time","daily","","24:00"}).ok(),"invalid time accepted");
 check(valid_date("2028-02-29")&&!valid_date("2100-02-29")&&!valid_date("2026-13-01")&&!valid_date("2026-11-00"),"date/leap validation failed");
 for(const auto& date:{"2026-10-31","2026-11-01","2026-12-31","2027-01-01"}) {
   check(store.set_completed(daily.id,date,true).ok(),"daily completion save failed");
   check(store.set_completed(daily.id,date,true).ok(),"completion not idempotent");
   check(store.snapshot(date).chores[0].completed,"completion missing for occurrence");
 }
 check(!store.snapshot("2026-11-02").chores[0].completed,"daily completion did not reset across DST boundary");
 check(!store.snapshot("2027-01-02").chores[0].completed,"daily completion did not reset across year boundary");
 check(!store.set_completed(once.id,"2026-11-02",true).ok(),"dated completion moved to another day");
 check(store.set_completed(once.id,"2026-11-01",true).ok(),"dated completion rejected");
 check(store.snapshot("2026-12-25").chores[1].completed,"dated completion did not persist");
 check(!due_today_or_overdue(store.snapshot("2026-11-02").chores[1],"2026-11-02"),"completed past chore stayed on today's board");
 check(store.set_completed(once.id,"2026-11-01",false).ok()&&due_today_or_overdue(store.snapshot("2026-11-02").chores[1],"2026-11-02"),"incomplete overdue item not visible");
 check(store.set_completed(once.id,"2026-11-01",true).ok(),"dated completion restore failed");
 check(!due_today_or_overdue(store.snapshot("2026-10-30").chores[1],"2026-10-30"),"future task visible as due");
 check(store.set_completed(daily.id,"2026-11-01",false).ok()&&!store.snapshot("2026-11-01").chores[0].completed,"undo completion failed");
 check(store.save_chore({once.id,person,"Windows renamed","once","2026-11-01",""}).ok()&&store.snapshot("2026-11-01").chores[1].completed,"rename erased completion");
 check(store.save_chore({once.id,person,"Windows rescheduled","once","2026-11-03","10:15"}).ok()&&!store.snapshot("2026-11-03").chores[1].completed,"reschedule retained stale completion");
 Store reopened;check(reopened.open(path).ok(),"reopen failed");check(reopened.snapshot("2026-11-03").chores[1].time=="10:15","schedule did not survive reopen");
 check(!store.open(path+"/missing.db").ok()&&store.ready()&&store.snapshot("2026-11-03").people.size()==1,"failed reopen destroyed working store");
 sqlite3* db=nullptr;check(sqlite3_open(path.c_str(),&db)==SQLITE_OK,"fault database open failed");
 check(sqlite3_exec(db,"CREATE TRIGGER fail_update BEFORE UPDATE ON chores BEGIN SELECT RAISE(ABORT,'injected failure'); END",nullptr,nullptr,nullptr)==SQLITE_OK,"fault trigger failed");
 check(!store.save_chore({once.id,person,"Lost update","once","2026-11-03",""}).ok(),"failed SQL reported success");
 check(store.snapshot("2026-11-03").chores[1].title=="Windows rescheduled","failed write changed cache");
 sqlite3_exec(db,"DROP TRIGGER fail_update",nullptr,nullptr,nullptr);sqlite3_close(db);
 // A database writer can wait on SQLite without freezing display snapshots.
 check(sqlite3_open(path.c_str(),&db)==SQLITE_OK&&sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK,"busy-lock fixture failed");
 std::atomic<bool> attempted=false;std::jthread blocked([&]{attempted=true;store.set_completed(daily.id,"2026-11-17",true);});
 while(!attempted.load())std::this_thread::yield();
 std::this_thread::sleep_for(std::chrono::milliseconds(30));
 auto started=std::chrono::steady_clock::now();auto visible=store.snapshot("2026-11-17");
 const bool responsive=std::chrono::steady_clock::now()-started<std::chrono::milliseconds(100);
 sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(db);blocked.join();
 check(responsive&&visible.chores.size()==2,"SQLite write blocked the display snapshot");
 std::vector<std::jthread> threads;std::atomic<bool> failed=false;
 for(int i=0;i<4;++i)threads.emplace_back([&]{for(int n=0;n<8;++n){if(!store.set_completed(daily.id,"2026-11-17",n%2).ok())failed=true;auto s=store.snapshot("2026-11-17");if(s.chores.size()!=2)failed=true;}});
 threads.clear();check(!failed,"concurrent edits corrupted chores");
 check(store.delete_chore(daily.id).ok()&&store.delete_chore(once.id).ok()&&store.delete_person(person).ok(),"cleanup failed");
 auto next=store.save_person({0,"New person","#abcdef"});check(next.ok()&&next.id>person,"deleted person ID was reused");
}
static void test_http(){
 Store store;check(store.open(":memory:").ok(),"API store open failed");
 auto call=[&](const std::string& path,const std::string& body){return handle_http(store,"POST",path,request(body),"2026-11-17","America/Los_Angeles");};
 check(call("/api/chores/people/save","name=Alice%20%26%20Bob&color=%2338bdf8").status==200,"HTTP person create failed");
 check(call("/api/chores/tasks/save","person_id=1&title=Feed%20cat&repeat=daily&all_day=1").status==200,"HTTP chore create failed");
 auto state=handle_http(store,"GET","/api/chores","","2026-11-17","America/Los_Angeles");
 check(state.status==200&&state.body.find("Alice & Bob")!=std::string::npos&&state.body.find("\"all_day\":true")!=std::string::npos,"HTTP snapshot invalid");
 check(call("/api/chores/tasks/complete","id=1&date=2026-11-16&completed=1").status==409,"stale daily board completed yesterday's occurrence");
 check(call("/api/chores/tasks/complete","id=1&date=2026-11-17&completed=1").status==200,"HTTP complete failed");
 check(call("/api/chores/people/delete","id=1").status==409,"HTTP lost assignee constraint");
 check(call("/api/chores/tasks/save","person_id=1&title=bad%00name&repeat=daily&all_day=1").status==400,"HTTP null accepted");
 check(call("/api/chores/tasks/save","id=999&person_id=1&title=Nope&repeat=once&date=2026-11-17&all_day=0&time=12:00").status==404,"stale edit inserted a chore");
 check(call("/api/chores/tasks/save","person_id=1&title=Bad&repeat=once&date=2026-11-31&all_day=0&time=25:00").status==400,"HTTP invalid schedule accepted");
 check(handle_http(store,"GET","/api/chores/tasks/delete","","2026-11-17","UTC").status==405,"GET mutated chores");
 check(call("/api/chores/tasks/delete_suffix","id=1").status==404,"suffix matched destructive route");
 auto queried=handle_http(store,"POST","/api/chores/tasks/delete",request("id=999","?id=1"),"2026-11-17","UTC");
 check(queried.status==404&&store.snapshot("2026-11-17").chores.size()==1,"query overrode POST body");
 auto cross=request("id=1");cross.insert(cross.find("Content-Type:"),"Origin: https://another-site.example\r\n");
 check(handle_http(store,"POST","/api/chores/tasks/delete",cross,"2026-11-17","UTC").status==403,"cross-origin form deleted a chore");
 check(call("/api/chores/tasks/delete","id=1").status==200&&call("/api/chores/people/delete","id=1").status==200,"HTTP delete failed");
}
int main(){char tmp[]="/tmp/pitrove-chores-XXXXXX";char* dir=mkdtemp(tmp);try{check(dir,"mkdtemp failed");test_geometry();test_store(std::string(dir)+"/chores.db");test_http();std::filesystem::remove_all(dir);std::cout<<"Chore persistence, scheduling, HTTP and panel geometry regressions passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';if(dir)std::filesystem::remove_all(dir);return 1;}}
