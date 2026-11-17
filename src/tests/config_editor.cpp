#include "config_editor.h"
#include "auth.h"
#include "config_text.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unistd.h>
using config_editor::apply;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::map<std::string,std::string> edit(const Config& c,std::map<std::string,std::string> fields){fields["revision"]=config_editor::revision(c);return fields;}
int main(){const auto dir=std::filesystem::temp_directory_path()/ ("pitrove-config-editor-"+std::to_string(getpid()));std::filesystem::create_directories(dir);const auto path=(dir/"config.toml").string();
 try{
 Config c;c.timezone="UTC";c.http_api_key="private-controller-key";c.mqtt_pass="private-mqtt-pass";c.gcalendar_api_key="private-calendar-key";c.gcalendar_ical_url="https://example.test/private-token/basic.ics";c.google_photos_refresh_token="private-refresh-token";
 const auto description=config_editor::describe(c);
 for(const auto& secret:{c.http_api_key,c.mqtt_pass,c.gcalendar_api_key,c.gcalendar_ical_url,c.google_photos_refresh_token})require(description.find(secret)==std::string::npos,"Schema leaked a configured secret");
 require(description.find("display.resolution")!=std::string::npos&&description.find("keepalive.gateway_ip")!=std::string::npos&&description.find("video.max_bitrate")!=std::string::npos,"Full schema omitted config sections");
 auto result=apply(c,edit(c,{{"display.resolution","2560,1440"},{"news.blacklist","[\"a,b.test\",\"quoted\\\".test\"]"},{"video.max_width","3840"},{"video.max_bitrate","42000000"},{"mqtt.api_key","wrong-scope"}}),path);
 require(result.status==400&&c.screen_w==1920&&!std::filesystem::exists(path),"Unknown field partially changed config");
 for(const auto& value:{"nan","inf","120oops","-3"})require(apply(c,edit(c,{{"slideshow.transition_delay",value}}),path).status==400,"Malformed number accepted");
 require(apply(c,edit(c,{{"infopanels.left_panel_max_chores","21"}}),path).status==400,"Range violation accepted");
 require(apply(c,edit(c,{{"brightness.auto_min","90"},{"brightness.auto_max","20"}}),path).status==400,"Inconsistent brightness accepted");
 require(apply(c,edit(c,{{"overlay.play_just_photos","1"},{"overlay.play_just_videos","1"}}),path).status==400,"Mutually exclusive playback accepted");
 require(apply(c,edit(c,{{"mqtt.timezone","Not/A_Zone"}}),path).status==400,"Invalid timezone accepted");
 const auto old=config_editor::revision(c);
 result=apply(c,edit(c,{{"display.resolution","2560,1440"},{"news.blacklist","[\"a,b.test\",\"quoted\\\".test\"]"},{"video.max_width","3840"},{"video.max_bitrate","42000000"},{"mqtt.pass",""},{"display.vignette_strength","0.8"}}),path);
 require(result.status==200&&c.screen_w==2560&&c.video_max_width==3840&&c.mqtt_pass=="private-mqtt-pass","Valid full config edit failed");
 Config loaded;require(loaded.load(path),"Saved config could not reload");require(loaded.video_max_width==3840&&loaded.video_max_bitrate==42000000&&loaded.news_blacklist==c.news_blacklist&&loaded.mqtt_pass==c.mqtt_pass,"Saved fields did not round trip");
 require(apply(c,{{"revision",old},{"slideshow.shuffle","0"}},path).status==409,"Stale configuration overwrote a newer edit");
 const auto good=config_editor::revision(c);require(apply(c,edit(c,{{"slideshow.shuffle","0"}}),(dir/"missing"/"config.toml").string()).status==500&&config_editor::revision(c)==good,"Failed save published config");
 require(apply(c,edit(c,{{"clear.mqtt.pass","1"}}),path).status==200&&c.mqtt_pass.empty(),"Explicit secret clear failed");
 require(apply(c,edit(c,{{"remote.api_key","new-controller-key"},{"gcalendar.api_key","new-calendar-key"}}),path).status==200&&c.http_api_key=="new-controller-key"&&c.gcalendar_api_key=="new-calendar-key","Same-named API keys crossed sections");
 require(apply(c,edit(c,{{"dashboard.dashboard_pin","1234"}}),path).status==200&&c.dashboard_pin.empty()&&!c.pin_hash.empty()&&pitrove::auth::verify_pin("1234",c.pin_hash),"PIN was not safely hashed on save");
 const auto retained_hash=c.pin_hash;require(apply(c,edit(c,{{"dashboard.dashboard_pin",""}}),path).status==200&&c.pin_hash==retained_hash,"Blank PIN edit erased the stored hash");
 require(apply(c,edit(c,{{"clear.dashboard.dashboard_pin","1"}}),path).status==200&&c.pin_hash.empty(),"Clearing a hashed PIN did not persist");
 // Legacy external secrets bootstrap correctly, then an atomic save is authoritative.
 {std::ofstream f(dir/"secrets.toml");f<<"[remote]\napi_key = \"old-bootstrap-key\"\n[mqtt]\npassword = \"old-bootstrap-pass\"\n";}
 require(loaded.load(path)&&loaded.http_api_key=="new-controller-key"&&loaded.mqtt_pass.empty(),"Stale external secrets overrode an explicit saved edit");
 require(apply(c,edit(c,{{"stockstreamer.preset","big_tech"}}),path).status==200&&c.stockstreamer_symbols.front()=="AAPL","Stock preset did not update its symbols");
 const auto form=[&](const std::string& fields,const std::string& query=""){return "POST /api/config"+query+" HTTP/1.1\r\nHost: frame.local\r\nOrigin: http://frame.local\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: "+std::to_string(fields.size())+"\r\n\r\n"+fields;};
 std::string fields="revision="+config_editor::revision(c)+"&infopanels.left_panel_max_chores=13";
 require(config_editor::handle(c,"POST",form(fields,"?infopanels.left_panel_max_chores=19"),path).status==200&&c.left_panel_max_chores==13,"Query overrode posted config");
 fields="revision="+config_editor::revision(c)+"&slideshow.shuffle=0&slideshow.shuffle=1";
 require(config_editor::handle(c,"POST",form(fields),path).status==400,"Duplicate field accepted");
 auto raw=form("revision="+config_editor::revision(c));raw.replace(raw.find("Origin: http://frame.local"),25,"Origin: http://elsewhere");
 require(config_editor::handle(c,"POST",raw,path).status==403,"Cross-origin config update accepted");
 require(config_editor::handle(c,"PUT","",path).status==405,"Invalid method accepted");
 std::filesystem::remove_all(dir);std::cout<<"Complete configuration: validation, atomic save, revision conflicts, secret/PIN preservation, presets and request protections passed\n";return 0;
 }catch(const std::exception& e){std::filesystem::remove_all(dir);std::cerr<<e.what()<<'\n';return 1;}}
