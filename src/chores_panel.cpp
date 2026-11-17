#include "chores_panel.h"
#include "chores.h"
#include "config.h"
#include "font_render.h"
#include "util.h"
#include <algorithm>
#include <sstream>
#include <tuple>
#include <cstdlib>
namespace {
std::string fit_text(FontRenderer* fr, const FontHandle& font, std::string text, int width) {
    int w=0,h=0; fr->measure(font,text,w,h);
    if (w<=width) return text;
    while (!text.empty()) {
        size_t end=text.size()-1;
        while (end && (static_cast<unsigned char>(text[end])&0xc0)==0x80) --end;
        text.resize(end); fr->measure(font,text+"…",w,h);
        if (w<=width) return text+"…";
    }
    return {};
}
std::vector<std::string> title_lines(FontRenderer* fr, const FontHandle& font, const std::string& title, int width) {
    std::istringstream words(title); std::string word,line; std::vector<std::string> lines;
    while (words>>word) {
        const auto next=line.empty() ? word : line+" "+word;
        int w,h; fr->measure(font,next,w,h);
        if (w>width && !line.empty() && lines.empty()) { lines.push_back(fit_text(fr,font,line,width)); line=word; }
        else line=next;
    }
    if (!line.empty()) lines.push_back(fit_text(fr,font,line,width));
    return lines;
}
}
void render_chores_panel(SDL_Renderer* renderer, FontRenderer* fonts, const std::string& path, const SDL_Rect& bounds, int screen_w) {
    if (!renderer || !fonts || bounds.w<=0 || bounds.h<=0) return;
    int max_items; bool completed,matting; std::string timezone;
    { std::shared_lock lock(g_config_mtx); max_items=g_cfg.left_panel_max_chores; completed=g_cfg.left_panel_show_completed; timezone=g_cfg.timezone; matting=g_cfg.matting; }
    auto px=[&](int value){return std::max(1,int(std::round(double(value)*screen_w/1920.0)));};
    struct State {
        uint64_t next=0; std::string zone,today; chores::Snapshot data;
        std::vector<chores::Chore> rows; bool ready=false, show_done=false; int total=0, done=0;
    };
    static State state;
    const uint64_t ticks=SDL_GetTicks();
    if (ticks>=state.next || timezone!=state.zone || completed!=state.show_done) {
        state.zone=timezone; state.today=format_epoch_tz(time(nullptr),timezone,"%Y-%m-%d");
        if (!chores::valid_date(state.today)) state.today=format_epoch_tz(time(nullptr),"UTC","%Y-%m-%d");
        state.ready=chores::g_store.ready(); state.data=chores::g_store.snapshot(state.today); state.next=ticks+1000;
        state.rows.clear(); state.total=0; state.done=0; state.show_done=completed;
    for (const auto& c:state.data.chores) if (chores::due_today_or_overdue(c,state.today)) {
        ++state.total; if (c.completed) ++state.done;
        if (completed || !c.completed) state.rows.push_back(c);
    }
    std::stable_sort(state.rows.begin(),state.rows.end(),[](const auto& a,const auto& b){
        return std::tuple{a.completed,a.repeat=="once" ? a.date : std::string("9999-99-99"),a.time,a.person_id,a.id}<
               std::tuple{b.completed,b.repeat=="once" ? b.date : std::string("9999-99-99"),b.time,b.person_id,b.id};
    });

    }
    SDL_Rect previous; SDL_GetRenderClipRect(renderer,&previous); const bool clipped=SDL_RenderClipEnabled(renderer);
    SDL_Rect clip=bounds;
    if (clipped) SDL_GetRectIntersection(&bounds,&previous,&clip);
    SDL_SetRenderClipRect(renderer,&clip);
    struct Restore { SDL_Renderer* r; SDL_Rect old; bool clipped; ~Restore(){SDL_SetRenderClipRect(r,clipped ? &old : nullptr);} } restore{renderer,previous,clipped};
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer,14,18,28,235);
    SDL_FRect bg{float(bounds.x),float(bounds.y),float(bounds.w),float(bounds.h)}; SDL_RenderFillRect(renderer,&bg);
    SDL_SetRenderDrawColor(renderer,56,189,248,140);
    SDL_FRect edge{float(bounds.x+bounds.w-px(2)),float(bounds.y),float(px(2)),float(bounds.h)}; SDL_RenderFillRect(renderer,&edge);
    const auto title=fonts->load_font(path,px(22)), body=fonts->load_font(path,px(14)), small=fonts->load_font(path,px(11));
    if (!title || !body || !small || !title->font || !body->font || !small->font) return;
    const int x=bounds.x+px(18), available=bounds.w-px(36);
    const int top=matting ? int(std::round(37.0*screen_w/1920.0)) : 0;
    int y=bounds.y+top+px(18);
    fonts->draw_text(x,y,*title,"Today's chores",255,255,255,255); y+=px(30);
    fonts->draw_text(x,y,*small,fit_text(fonts,*small,std::to_string(state.done)+" of "+std::to_string(state.total)+" done • "+state.today,available),56,189,248,255);
    y+=px(30);
    const auto& rows=state.rows;
    if (rows.empty()) {
        const std::string message=!state.ready ? "Chores unavailable" : state.total ? "All caught up!" : "No chores scheduled";
        fonts->draw_text(x,y,*body,fit_text(fonts,*body,message,available),190,205,225,255);
        fonts->draw_text(x,y+px(24),*small,fit_text(fonts,*small,"Add people and chores in Web UI",available),135,155,180,255);
        return;
    }
    const int card_h=px(86), slots=std::min(std::clamp(max_items,1,20),std::max(1,(bounds.y+bounds.h-y-px(36))/card_h));
    const size_t pages=(rows.size()+slots-1)/slots, page=pages ? (ticks/12000)%pages : 0;
    const size_t first=page*slots, last=std::min(rows.size(),first+slots);
    for (size_t i=first;i<last;++i) {
        const auto& c=rows[i];
        auto person=std::find_if(state.data.people.begin(),state.data.people.end(),[&](auto& p){return p.id==c.person_id;});
        const std::string name=person==state.data.people.end() ? "Unassigned" : person->name;
        unsigned color=0x38bdf8;
        if (person!=state.data.people.end()) color=static_cast<unsigned>(std::strtoul(person->color.c_str()+1,nullptr,16));
        SDL_SetRenderDrawColor(renderer,(color>>16)&255,(color>>8)&255,color&255,c.completed?90:230);
        SDL_FRect bar{float(x),float(y+px(2)),float(px(3)),float(card_h-px(12))}; SDL_RenderFillRect(renderer,&bar);
        int ty=y;
        for (const auto& line:title_lines(fonts,*body,c.title,available-px(14))) { fonts->draw_text(x+px(12),ty,*body,line,c.completed?130:240,c.completed?155:245,c.completed?150:255,255); ty+=px(18); }
        fonts->draw_text(x+px(12),ty+px(2),*small,fit_text(fonts,*small,name,available-px(14)),(color>>16)&255,(color>>8)&255,color&255,255);
        const auto when=c.completed ? "Done" : (c.repeat=="once" && c.date<state.today ? "Overdue • "+c.date : (c.time.empty() ? "All day" : "Due "+c.time));
        fonts->draw_text(x+px(12),ty+px(18),*small,fit_text(fonts,*small,when,available-px(14)),c.completed?130:180,c.completed?170:190,185,255);
        y+=card_h;
    }
    fonts->draw_text(x,bounds.y+bounds.h-px(25),*small,fit_text(fonts,*small,pages>1 ? "Page "+std::to_string(page+1)+" / "+std::to_string(pages)+" • Web UI to manage" : "Manage chores in Web UI",available),120,145,170,255);
}
