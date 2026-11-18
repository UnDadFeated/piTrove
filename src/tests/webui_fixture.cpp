#include "chores.h"
#include "chores_http.h"
#include "chores_page.h"
#include "dashboard_page.h"
#include "config_page.h"
#include "config_editor.h"
#include "http_protocol.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2)return 1;const std::string dir=argv[1];std::filesystem::create_directories(dir);
    if(!chores::g_store.open(dir+"/chores.db").ok())return 1;
    Config cfg;cfg.timezone="America/Los_Angeles";cfg.media_dir=dir;cfg.cache_dir=dir;cfg.log_dir=dir;cfg.http_api_key="chores-fixture-key";
    const std::string config_path=dir+"/config.toml";if(std::filesystem::exists(config_path))cfg.load(config_path);else if(!cfg.save(config_path))return 1;
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0),yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(19030);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0||listen(fd,16)!=0)return 2;
    bool paused=false,shuffle=true;int index=0;
    while(true){int client=accept(fd,nullptr,nullptr);if(client<0)break;std::string raw;http_protocol::Request req;int status=200;std::string body,mime="application/json";
      if(http_protocol::receive(client,raw,req,2000)!=http_protocol::Result::complete){close(client);continue;}
      if(req.path=="/"||req.path=="/dashboard"){body=get_dashboard_html("18.4.1-test");mime="text/html; charset=utf-8";}
      else if(req.path=="/chores"){body=get_chores_page("18.4.1-test");mime="text/html; charset=utf-8";}
      else if(req.path=="/config"){body=get_config_page("18.4.1-test");mime="text/html; charset=utf-8";}
      else if(req.path=="/api/preview"){body="<svg xmlns='http://www.w3.org/2000/svg' width='800' height='500'><rect width='800' height='500' fill='#274943'/><path d='M0 500 280 160 420 340 620 130 800 500' fill='#68ad8c'/><circle cx='620' cy='100' r='45' fill='#edcf8d'/></svg>";mime="image/svg+xml";}
      else if(req.path=="/api/status"){body="{\"filename\":\"A long family photo filename & <safe text> — 2026.jpg\",\"index\":"+std::to_string(index)+",\"total\":38,\"is_video\":false,\"shuffle\":"+(shuffle?"true":"false")+",\"paused\":"+(paused?"true":"false")+",\"uptime\":\"4d 2h\",\"temp\":\"48.2°C\",\"mem\":\"42%\",\"disk\":\"38%\",\"db_size\":\"24.8 MB\",\"mqtt_status\":\"enabled\",\"mqtt_broker\":\"test.local\",\"mqtt_port\":1883,\"screen_blanked\":false,\"item_timer\":25,\"transition_delay\":120}";}
      else if(req.headers["authorization"]!="Bearer "+cfg.http_api_key&&!cfg.http_api_key.empty()){status=401;body="{\"error\":\"Unauthorized\"}";}
      else if(req.path=="/api/config"){auto result=config_editor::handle(cfg,req.method,raw,config_path);status=result.status;body=std::move(result.body);}
      else if(req.path=="/api/settings")body=std::string("{\"left_panel_enabled\":")+(cfg.left_panel_enabled?"true":"false")+",\"left_panel_clock_enabled\":"+(cfg.left_panel_clock_enabled?"true":"false")+",\"infopanels_enabled\":true}";
      else if(req.path=="/api/settings/update"){if(auto value=http_protocol::query_param(raw,"left_panel_enabled"))cfg.left_panel_enabled=*value=="1";if(auto value=http_protocol::query_param(raw,"left_panel_clock_enabled"))cfg.left_panel_clock_enabled=*value=="1";body="{\"status\":\"ok\"}";}
      else if(req.path=="/api/logs")body="{\"logs\":\"INFO Playback started\\nWARN Example warning\\nINFO <img src=x onerror=alert(1)> safe log text\\n\"}";
      else if(req.path=="/api/pause"){paused=!paused;body="{\"status\":\"ok\"}";}
      else if(req.path=="/api/next"){index=(index+1)%38;body="{\"status\":\"ok\"}";}
      else if(req.path=="/api/prev"){index=(index+37)%38;body="{\"status\":\"ok\"}";}
      else if(req.path=="/api/toggle_shuffle"){shuffle=!shuffle;body="{\"status\":\"ok\"}";}
      else{auto result=chores::handle_http(chores::g_store,req.method,req.path,raw,"2026-11-17","America/Los_Angeles");status=result.status;body=std::move(result.body);}
      const std::string header="HTTP/1.1 "+std::to_string(status)+" Result\r\nContent-Type: "+mime+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n";
      if(http_protocol::send_all(client,header))http_protocol::send_all(client,body);close(client);
    }close(fd);
}
