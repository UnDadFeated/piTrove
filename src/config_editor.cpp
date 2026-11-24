#include "config_editor.h"
#include "config_text.h"
#include "http_protocol.h"
#include "stock_presets.h"
#include "util.h"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>

namespace config_editor {
namespace {
std::string quote(const std::string& value) { return "\""+config_text::escape(value)+"\""; }
struct Field {
    std::string id,label,group,help,kind;
    double min,max;
    std::vector<std::string> choices;
    bool restart,secret;
    std::function<std::string(const Config&)> get;
    std::function<bool(Config&,const std::string&)> set;
};
template<class T> std::string number(T value) {
    std::array<char,128> buffer{};
    const auto result=[&]{if constexpr(std::is_floating_point_v<T>) return std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,std::chars_format::general,std::numeric_limits<T>::max_digits10);else return std::to_chars(buffer.data(),buffer.data()+buffer.size(),value);}();
    return std::string(buffer.data(),result.ptr);
}
template<class T> Field make(std::string id,std::string label,std::string group,std::string help,T Config::*member,double min,double max,std::vector<std::string> choices={},bool restart=true,bool secret=false) {
    std::string kind;
    if constexpr(std::is_same_v<T,bool>)kind="boolean";
    else if constexpr(std::is_integral_v<T>)kind="integer";
    else if constexpr(std::is_floating_point_v<T>)kind="number";
    else if constexpr(std::is_same_v<T,std::vector<std::string>>)kind="list";
    else kind="string";
    return {std::move(id),std::move(label),std::move(group),std::move(help),kind,min,max,choices,restart,secret,
      [member](const Config& c)->std::string {
        const auto& v=c.*member;
        if constexpr(std::is_same_v<T,std::string>)return quote(v);
        else if constexpr(std::is_same_v<T,bool>)return v?"true":"false";
        else if constexpr(std::is_same_v<T,std::vector<std::string>>){std::string out="[";for(const auto& item:v){if(out.size()>1)out+=',';out+=quote(item);}return out+"]";}
        else return number(v);
      },
      [member,min,max,choices](Config& c,const std::string& text)->bool {
        if constexpr(std::is_same_v<T,std::string>){
          if(text.size()>8192||std::any_of(text.begin(),text.end(),[](unsigned char ch){return ch<32||ch==127;}))return false;
          if(!choices.empty()&&std::find(choices.begin(),choices.end(),text)==choices.end()&&text!=c.*member)return false;
          c.*member=text;return true;
        }else if constexpr(std::is_same_v<T,bool>){if(text!="0"&&text!="1"&&text!="true"&&text!="false")return false;c.*member=text=="1"||text=="true";return true;}
        else if constexpr(std::is_same_v<T,std::vector<std::string>>){
          if(text.size()>32768)return false;
          auto list=config_text::array(text);
          if(!list||list->size()>512)return false;
          for(const auto& value:*list)if(value.size()>2048||std::any_of(value.begin(),value.end(),[](unsigned char ch){return ch<32||ch==127;}))return false;
          c.*member=std::move(*list);return true;
        }else{
          T value{};auto result=std::from_chars(text.data(),text.data()+text.size(),value);
          if(result.ec!=std::errc{}||result.ptr!=text.data()+text.size()||!std::isfinite(static_cast<double>(value))||value<static_cast<T>(min)||value>static_cast<T>(max))return false;
          c.*member=value;return true;
        }
      }};
}
Field resolution_field() {
    return {"display.resolution","Resolution","Display","Output dimensions in pixels: width,height. Presets match the TUI; custom sizes are supported.","string",0,0,{"1920,1080","2560,1440","3840,2160"},true,false,
      [](const Config& c){return quote(std::to_string(c.screen_w)+","+std::to_string(c.screen_h));},
      [](Config& c,const std::string& text){const auto comma=text.find(',');if(comma==text.npos)return false;int width=0,height=0;const auto a=std::from_chars(text.data(),text.data()+comma,width),b=std::from_chars(text.data()+comma+1,text.data()+text.size(),height);if(a.ec!=std::errc{}||b.ec!=std::errc{}||a.ptr!=text.data()+comma||b.ptr!=text.data()+text.size()||width<320||width>16384||height<240||height>16384)return false;c.screen_w=width;c.screen_h=height;return true;}};
}
const std::vector<Field>& schema() {
    static const std::vector<Field> fields={
#include "config_fields.inc"
    };
    return fields;
}
Result error(int status,const std::string& message) { return {status,"{\"error\":"+quote(message)+"}",{}}; }
bool time_valid(const std::string& s) {
    return s.empty()||(s.size()==5&&s[2]==':'&&s[0]>='0'&&s[0]<='2'&&s[1]>='0'&&s[1]<='9'&&s[3]>='0'&&s[3]<='5'&&s[4]>='0'&&s[4]<='9'&&s.substr(0,2)<="23");
}
}
bool is_valid_timezone(const std::string& tz) {
    if (tz.empty() || tz.find("..") != std::string::npos || tz.front() == '/' || tz.size() > 128) return false;
    std::ifstream zone("/usr/share/zoneinfo/" + tz, std::ios::binary);
    char header[4]{};
    zone.read(header, 4);
    return static_cast<bool>(zone) && std::string_view(header, 4) == "TZif";
}
std::string revision(const Config& c) {
    static const auto key=[] {
      if (sodium_init() < 0) throw std::runtime_error("libsodium initialization failed");
      std::array<unsigned char,32> key{};
      randombytes_buf(key.data(),key.size());
      return key;
    }();
    crypto_generichash_state state;crypto_generichash_init(&state,key.data(),key.size(),32);
    for(const auto& f:schema()){const auto data=f.id+"="+f.get(c)+"\n";crypto_generichash_update(&state,reinterpret_cast<const unsigned char*>(data.data()),data.size());}
    std::array<unsigned char,32> digest{};crypto_generichash_final(&state,digest.data(),digest.size());
    std::array<char,65> out{};sodium_bin2hex(out.data(),out.size(),digest.data(),digest.size());return out.data();
}
std::vector<FieldView> fields(const Config& config) {
    std::vector<FieldView> result;
    result.reserve(schema().size());
    for (const auto& field : schema()) {
        auto value = field.get(config);
        const bool configured = field.secret && (value != "\"\"" ||
            (field.id == "dashboard.dashboard_pin" && !config.pin_hash.empty()));
        if (field.secret) value.clear();
        else if (field.kind == "string") value = config_text::decode(value).value_or("");
        result.push_back({field.id, field.label, field.group, field.help, field.kind,
                          std::move(value), field.choices, field.secret, configured});
    }
    return result;
}

std::string describe(const Config& c) {
    std::string out="{\"revision\":"+quote(revision(c))+",\"fields\":[";
    for(const auto& f:schema()){
        if(out.back()!='[')out+=',';
        out+="{\"id\":"+quote(f.id)+",\"label\":"+quote(f.label)+",\"group\":"+quote(f.group)+",\"help\":"+quote(f.help)+",\"type\":"+quote(f.kind)+",\"value\":"+(f.secret?"\"\"":f.get(c))+",\"secret\":"+(f.secret?"true":"false")+",\"configured\":"+(f.secret&&(f.get(c)!="\"\""||(f.id=="dashboard.dashboard_pin"&&!c.pin_hash.empty()))?"true":"false")+",\"restart\":"+(f.restart?"true":"false")+",\"min\":"+number(f.min)+",\"max\":"+number(f.max)+",\"choices\":[";
        for(const auto& v:f.choices){if(out.back()!='[')out+=',';out+=quote(v);}out+="]}";
    }
    return out+"]}";
}
Result apply(Config& config,const std::map<std::string,std::string>& values,const std::string& path) {
    const auto rev=values.find("revision");
    if(rev==values.end())return error(400,"Reload the full configuration before saving.");
    if(rev->second!=revision(config))return error(409,"Configuration changed on the frame. Reload it, then review your edits again.");
    Config candidate = config;
    for(const auto& [id,value]:values){
      if(id=="revision")continue;
      const bool clear=id.starts_with("clear.");const auto key=clear?id.substr(6):id;
      auto it=std::find_if(schema().begin(),schema().end(),[&](const Field& f){return f.id==key;});
      if(it==schema().end())return error(400,"Unknown configuration option: "+key);
      if(clear&&(!it->secret||value!="1"||values.contains(key)))return error(400,"Choose one replacement or clear action for "+it->label+".");
      if(it->secret&&!clear&&value.empty())continue; // Blank secret inputs preserve the configured value.
      if(!it->set(candidate,clear?"":value))return error(400,"Choose a valid value for "+it->label+".");
    }
    if(candidate.brightness_auto_min>candidate.brightness_auto_max)return error(400,"Minimum brightness cannot exceed maximum brightness.");
    if(candidate.play_just_photos&&candidate.play_just_videos)return error(400,"Choose photos only or videos only, not both.");
    if(!time_valid(candidate.sleep_time)||!time_valid(candidate.wake_time))return error(400,"Sleep and wake times use HH:MM (24-hour time), or leave them empty.");
    if(candidate.rotation!=0&&candidate.rotation!=90&&candidate.rotation!=180&&candidate.rotation!=270)return error(400,"Rotation must be 0, 90, 180 or 270 degrees.");
    if(candidate.timezone.empty())return error(400,"Display timezone cannot be empty.");
    if(!is_valid_timezone(candidate.timezone))return error(400,"Choose an installed display timezone, such as America/Los_Angeles.");
    if(!candidate.keepalive_gateway.empty()&&!is_valid_host_or_ip(candidate.keepalive_gateway))return error(400,"Keepalive gateway must be a valid IP address or hostname.");
    const auto has=[&](const std::string& id){
      if(values.contains("clear."+id))return true;
      const auto value=values.find(id);if(value==values.end())return false;
      const auto field=std::find_if(schema().begin(),schema().end(),[&](const Field& f){return f.id==id;});
      return field==schema().end()||!field->secret||!value->second.empty();
    };
    if(has("dashboard.dashboard_pin")){
      if(has("dashboard.pin_hash"))return error(400,"Set a new PIN or a stored PIN hash, not both.");
      if(!candidate.dashboard_pin.empty()&&(candidate.dashboard_pin.size()!=4||!std::all_of(candidate.dashboard_pin.begin(),candidate.dashboard_pin.end(),[](unsigned char c){return c>='0'&&c<='9';})))return error(400,"The touch dashboard PIN must have four digits.");
      candidate.pin_hash.clear();candidate.pin_changed=true;
    }
    if(has("dashboard.pin_hash")){
      if(!candidate.pin_hash.empty()&&crypto_pwhash_str_needs_rehash(candidate.pin_hash.c_str(),crypto_pwhash_OPSLIMIT_INTERACTIVE,crypto_pwhash_MEMLIMIT_INTERACTIVE)<0)return error(400,"The stored PIN hash is not a valid Argon2 hash.");
      candidate.dashboard_pin.clear();candidate.pin_changed=true;
    }
    if(has("stockstreamer.symbols")&&!values.contains("stockstreamer.preset"))candidate.stockstreamer_preset="custom";
    if(values.contains("stockstreamer.preset")&&candidate.stockstreamer_preset!="custom"&&!has("stockstreamer.symbols"))candidate.stockstreamer_symbols=get_stock_preset_symbols(candidate.stockstreamer_preset);
    std::vector<std::string> changed;
    bool restart = false;
    for(const auto& f:schema())if(f.get(candidate)!=f.get(config)){changed.push_back(f.id);restart=restart||f.restart;}
    if(changed.empty())return {200,"{\"status\":\"ok\",\"restart_required\":false,\"revision\":"+quote(revision(config))+"}",{}};
    if(!candidate.save(path))return error(500,"Configuration could not be saved. Your previous settings are still active.");
    config=std::move(candidate);
    return {200,"{\"status\":\"ok\",\"restart_required\":"+std::string(restart?"true":"false")+",\"revision\":"+quote(revision(config))+"}",std::move(changed)};
}
Result handle(Config& config,const std::string& method,const std::string& raw,const std::string& path) {
    if(method=="GET")return {200,describe(config),{}};
    if(method!="POST")return error(405,"Use GET to read configuration and POST to save changes.");
    http_protocol::Request request;size_t required;
    if(http_protocol::parse(raw,request,required)!=http_protocol::Result::complete)return error(400,"Incomplete configuration request.");
    const auto type=request.headers.find("content-type");
    if(type==request.headers.end()||type->second.substr(0,type->second.find(';'))!="application/x-www-form-urlencoded")return error(415,"Send form-encoded configuration changes.");
    if (!http_protocol::controller_origin_allowed(request))
        return error(403, "Open the configuration from your piTrove controller to save changes.");
    std::string_view body(raw.data()+raw.find("\r\n\r\n")+4,request.content_length);std::map<std::string,std::string> fields;
    // Decode each posted pair separately; query parameters never override posted values.
    while(!body.empty()){
      const auto end=body.find('&');const auto pair=body.substr(0,end);const auto eq=pair.find('=');
      if(eq==pair.npos)return error(400,"Invalid configuration form.");
      const auto id = http_protocol::decode_form(pair.substr(0, eq));
      const auto value = http_protocol::decode_form(pair.substr(eq + 1));
      if(!fields.emplace(id,value).second)return error(400,"Duplicate configuration option: "+id);
      if(end==body.npos)break;
      body.remove_prefix(end+1);
    }
    return apply(config,fields,path);
}
} // namespace config_editor
