#pragma once
#include <SDL3/SDL_rect.h>
#include <algorithm>
#include <cmath>
struct InfopanelLayout {
    SDL_Rect slideshow_area{}, calendar_area{}, stocks_area{}, news_area{}, chores_area{};
    int content_top=0;
    bool show_calendar=false, show_stocks=false, show_news=false, show_chores=false;
};
struct InfopanelOptions {
    bool enabled=false, news=false, calendar=false, stocks=false, left=false, matting=false;
    int matting_size=0, offset_x=0, offset_y=0;
};
// One geometry source for photos, twin portraits, transitions, video and every panel.
inline InfopanelLayout make_infopanel_layout(int width, int height, const InfopanelOptions& o) {
    InfopanelLayout l;
    if (width<=0 || height<=0) return l;
    auto sx=[&](double x) { return int(std::round(x*width/1920.0)); };
    auto sy=[&](double y) { return int(std::round(y*height/1080.0)); };
    const int ox=std::clamp(o.offset_x,-width/4,width/4), oy=std::clamp(o.offset_y,-height/4,height/4);
    const int inset=o.matting ? sx(60) : 0;
    const int left=inset+ox, right=width-inset+ox;
    const int top=(o.matting ? sy(37) : 0)+oy;
    l.content_top=top;
    const int bottom=(o.matting ? height-sy(37) : height)+oy;
    l.show_calendar=o.enabled && o.calendar; l.show_stocks=o.enabled && o.stocks;
    l.show_news=o.enabled && o.news; l.show_chores=o.enabled && o.left;
    const bool right_on=l.show_calendar || l.show_stocks;
    l.show_news = l.show_news && bottom - top >= 2;
    const int preferred_news_bottom =
        (o.matting ? height - int(std::round(sx(o.matting_size) * 0.8)) + sx(45) : height) + oy;
    const int news_bottom = l.show_news ? std::clamp(preferred_news_bottom, top + 2, bottom) : bottom;
    const int news_h = l.show_news ? std::clamp(sx(36), 1, news_bottom - top - 1) : 0;
    const int content_bottom=l.show_news ? std::max(top+1,news_bottom-news_h) : bottom;
    // Both visible rails are 275px inside the physical matte (335px without it).
    const int rail=std::clamp(sx(335)-inset,1,std::max(1,(right-left)/4));
    if (l.show_chores) l.chores_area={left,oy,rail,std::max(1,content_bottom-oy)};
    const int rx=right-rail;
    if (content_bottom - oy < 2 && l.show_calendar) l.show_stocks = false;
    if (l.show_calendar && l.show_stocks) {
        // Keep the split inside the usable height, including wide, short displays.
        const int split = std::min(content_bottom - 1, std::max(top + sx(40), content_bottom - sx(288)));
        l.calendar_area={rx,oy,rail,std::max(1,split-oy)};
        l.stocks_area={rx,split,rail,std::max(1,content_bottom-split)};
    } else if (l.show_calendar) l.calendar_area={rx,oy,rail,std::max(1,content_bottom-oy)};
    else if (l.show_stocks) l.stocks_area={rx,oy,rail,std::max(1,content_bottom-oy)};
    if (l.show_news) l.news_area={ox,content_bottom,width,news_h};
    const int x1=left+(l.show_chores ? rail : 0), x2=right-(right_on ? rail : 0);
    const int shift=sy(5);
    l.slideshow_area={x1,top,std::max(1,x2-x1),std::max(1,content_bottom-top-shift)};
    return l;
}

// The legacy 72px anchor included 60px of physical matte plus 12px of padding.
// At a chores-column border only the 12px padding remains; offsets are already
// included in media_left and must not be applied a second time.
inline int filename_overlay_x(int width, int offset_x, bool left_panel_shown, int media_left) {
    if (left_panel_shown) return media_left + int(std::round(12.0 * width / 1920.0));
    return int(std::round(72.0 * width / 1920.0)) + std::clamp(offset_x, -width/4, width/4);
}
