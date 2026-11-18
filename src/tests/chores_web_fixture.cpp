// Loopback adapter for browser tests; all chore mutations use the production handler.
#include "chores.h"
#include "chores_http.h"
#include "chores_page.h"
#include "http_protocol.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=2||!chores::g_store.open(argv[1]).ok())return 1;
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0),yes=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(19030);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0||listen(fd,16)!=0)return 2;
    bool panel=false,clock=true;
    while(true){
        int client=accept(fd,nullptr,nullptr);if(client<0)break;
        std::string raw;http_protocol::Request request;int status=200;std::string body,mime="application/json";
        if(http_protocol::receive(client,raw,request,2000)!=http_protocol::Result::complete){close(client);continue;}
        if(request.path=="/chores"){body=get_chores_page();mime="text/html; charset=utf-8";}
        else if(request.headers["authorization"]!="Bearer chores-fixture-key"){status=401;body="{\"error\":\"Unauthorized\"}";}
        else if(request.path=="/api/settings")body=std::string("{\"left_panel_enabled\":")+(panel?"true":"false")+",\"left_panel_clock_enabled\":"+(clock?"true":"false")+",\"infopanels_enabled\":true}";
        else if(request.path=="/api/settings/update"){if(auto value=http_protocol::query_param(raw,"left_panel_enabled"))panel=*value=="1";if(auto value=http_protocol::query_param(raw,"left_panel_clock_enabled"))clock=*value=="1";body="{\"status\":\"ok\"}";}
        else{auto result=chores::handle_http(chores::g_store,request.method,request.path,raw,"2026-11-17","America/Los_Angeles");status=result.status;body=std::move(result.body);}
        const std::string header="HTTP/1.1 "+std::to_string(status)+" Result\r\nContent-Type: "+mime+"\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n";
        if(http_protocol::send_all(client,header))http_protocol::send_all(client,body);close(client);
    }
    close(fd);
}
