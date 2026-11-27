#include "chores_panel.h"
#include "chores.h"
#include "config.h"
#include "font_render.h"
#include "infopanel_render.h"
#include "util.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string_view>
#include <tuple>

namespace {
std::vector<std::string> title_lines(FontRenderer& fonts, const FontHandle& font,
                                     const std::string& title, int width) {
    std::istringstream words(title);
    std::string word, line;
    std::vector<std::string> lines;
    while (words >> word) {
        const auto next = line.empty() ? word : line + " " + word;
        int measured_width, height;
        fonts.measure(font, next, measured_width, height);
        if (measured_width > width && !line.empty() && lines.empty()) {
            lines.push_back(fonts.ellipsize(font, line, width));
            line = word;
        } else {
            line = next;
        }
    }
    if (!line.empty()) lines.push_back(fonts.ellipsize(font, line, width));
    return lines;
}

auto chore_sort_key(const chores::Chore& chore) {
    const auto date = chore.repeat == "once" ? std::string_view(chore.date) : "9999-99-99";
    return std::tuple{chore.completed, date, std::string_view(chore.time), chore.person_id, chore.id};
}

bool same_card_data(const chores::Snapshot& before, const chores::Snapshot& after) {
    const auto person = [](const chores::Person& value) {
        return std::tie(value.id, value.name, value.color);
    };
    const auto chore = [](const chores::Chore& value) {
        return std::tie(value.id, value.person_id, value.title, value.repeat, value.date,
                        value.time, value.completed);
    };
    return std::ranges::equal(before.people, after.people, {}, person, person) &&
           std::ranges::equal(before.chores, after.chores, {}, chore, chore);
}

struct ChoreCard {
    std::vector<std::string> title;
    std::string name, schedule;
    uint8_t red, green, blue;
    bool completed;
};

}

void render_chores_panel(SDL_Renderer* renderer, FontRenderer* fonts, const std::string& path,
                         const SDL_Rect& bounds, int screen_w, int content_top) {
    if (!renderer || !fonts || bounds.w <= 0 || bounds.h <= 0) return;
    const InfopanelClip clip(renderer, bounds);
    if (!clip) return;

    int max_items;
    bool show_done, show_clock;
    std::string timezone;
    {
        std::shared_lock lock(g_config_mtx);
        max_items = g_cfg.left_panel_max_chores;
        show_done = g_cfg.left_panel_show_completed;
        show_clock = g_cfg.left_panel_clock_enabled;
        timezone = g_cfg.timezone;
    }
    const auto px = [screen_w](int value) { return panel_px(value, screen_w); };
    struct State {
        uint64_t next_refresh = 0;
        std::string zone, today, date, clock;
        chores::Snapshot data;
        std::vector<const chores::Chore*> rows;
        std::vector<ChoreCard> cards;
        std::tuple<FontRenderer*, std::string, int, int, int, size_t, int> layout;
        bool ready = false, show_done = false, show_clock = false;
        int total = 0, done = 0;
    };
    static State state;
    const uint64_t ticks = SDL_GetTicks();
    if (ticks >= state.next_refresh || timezone != state.zone ||
        show_done != state.show_done || show_clock != state.show_clock) {
        const auto now = time(nullptr);
        state.zone = timezone;
        auto today = format_epoch_tz(now, timezone, "%Y-%m-%d");
        state.show_clock = show_clock;
        if (show_clock) {
            state.date = format_epoch_tz(now, timezone, "%m/%d/%Y");
            state.clock = format_epoch_tz(now, timezone, "%I:%M %p");
            if (state.clock.starts_with('0')) state.clock.erase(0, 1);
        }
        state.ready = chores::g_store.ready();
        auto data = chores::g_store.snapshot(today);
        state.next_refresh = ticks + 1000;
        if (today != state.today || show_done != state.show_done || !same_card_data(state.data, data)) {
            state.today = std::move(today);
            state.show_done = show_done;
            state.cards.clear();
            state.rows.clear();
            state.data = std::move(data);
            state.total = 0;
            state.done = 0;
            for (const auto& chore : state.data.chores) {
                if (!chores::due_today_or_overdue(chore, state.today)) continue;
                ++state.total;
                if (chore.completed) ++state.done;
                if (show_done || !chore.completed) state.rows.push_back(&chore);
            }
            std::sort(state.rows.begin(), state.rows.end(), [](const auto* a, const auto* b) {
                return chore_sort_key(*a) < chore_sort_key(*b);
            });
        }
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 14, 18, 28, 235);
    const SDL_FRect backdrop{float(bounds.x), float(bounds.y), float(bounds.w), float(bounds.h)};
    SDL_RenderFillRect(renderer, &backdrop);
    SDL_SetRenderDrawColor(renderer, 56, 189, 248, 140);
    const SDL_FRect edge{float(bounds.x + bounds.w - px(2)), float(bounds.y), float(px(2)), float(bounds.h)};
    SDL_RenderFillRect(renderer, &edge);

    const int padding = px(18);
    const int available = bounds.w - 2 * padding;
    if (available <= 0) return;
    const auto heading = fonts->load_font(path, px(panel_heading_size));
    const auto body = fonts->load_font(path, px(panel_body_size));
    const auto detail = fonts->load_font(path, px(panel_detail_size));
    const int heading_step = TTF_GetFontHeight(heading->font) + px(6);
    const int body_step = TTF_GetFontHeight(body->font) + px(3);
    const int detail_step = TTF_GetFontHeight(detail->font) + px(4);
    const int summary_step = detail_step + px(10);
    const int card_height = 3 * body_step + detail_step + px(9); // two title lines, person, schedule
    const int x = bounds.x + padding;
    const int bottom = bounds.y + bounds.h;
    const int footer_y = bottom - detail_step - px(8);
    const int list_bottom = state.rows.size() > 1 ? footer_y : bottom - px(8);
    int y = std::max(content_top, bounds.y) + padding;
    bool date_drawn = false;
    if (show_clock && !state.clock.empty()) {
        const int clock_padding = px(8);
        const int heading_gap = px(10);
        const int list_space = state.rows.empty() ? body_step : card_height;
        const int header_height = list_bottom - y - heading_gap - heading_step - summary_step - list_space;
        const int header_width = bounds.w - 2 * clock_padding;
        if (header_width > 0 && header_height > 0) {
            const int header_x = bounds.x + clock_padding;
            const int date_gap = std::min(px(4), header_height / 7);
            const auto clock_font = fonts->load_font(path, px(88));
            int date_height = 0;
            const int date_limit = (header_height - date_gap) / 3;
            if (date_limit > 0 && !state.date.empty()) {
                date_height = fonts->draw_text_fitted({header_x, y, header_width, date_limit},
                                                       *clock_font, state.date, {205, 218, 236, 255});
                date_drawn = date_height > 0;
            }
            const int date_space = date_drawn ? date_height + date_gap : 0;
            const int clock_height = fonts->draw_text_fitted(
                {header_x, y + date_space, header_width, header_height - date_space},
                *clock_font, state.clock, {255, 255, 255, 255});
            if (date_space + clock_height > 0) y += date_space + clock_height + heading_gap;
        }
    }

    fonts->draw_text(x, y, *heading, fonts->ellipsize(*heading, "Today's chores", available),
                     255, 255, 255, 255);
    y += heading_step;
    auto summary = std::to_string(state.done) + " of " + std::to_string(state.total) + " done";
    if (!date_drawn) summary += " • " + state.today;
    fonts->draw_text(x, y, *detail, fonts->ellipsize(*detail, summary, available), 56, 189, 248, 255);
    y += summary_step;

    if (state.rows.empty()) {
        const std::string message = !state.ready ? "Chores unavailable" :
                                    state.total ? "All caught up!" : "No chores scheduled";
        fonts->draw_text(x, y, *body, fonts->ellipsize(*body, message, available), 190, 205, 225, 255);
        return;
    }

    const int slots = std::min(std::clamp(max_items, 1, 20), (list_bottom - y) / card_height);
    if (slots <= 0) return;

    const size_t pages = (state.rows.size() + slots - 1) / slots;
    const size_t page = (ticks / 12000) % pages;
    const size_t end = std::min(state.rows.size(), (page + 1) * slots);
    const int text_x = x + px(12);
    const int text_width = available - px(12);
    const auto layout = std::tuple{fonts, path, px(panel_body_size), px(panel_detail_size),
                                   text_width, page, slots};
    if (state.cards.empty() || state.layout != layout) {
        state.layout = layout;
        state.cards.clear();
        for (size_t i = page * slots; i < end; ++i) {
            const auto& chore = *state.rows[i];
            const auto person = std::find_if(state.data.people.begin(), state.data.people.end(),
                [&](const auto& p) { return p.id == chore.person_id; });
            const bool assigned = person != state.data.people.end();
            unsigned color = 0x38bdf8;
            if (assigned && person->color.size() == 7 && person->color.front() == '#')
                color = static_cast<unsigned>(std::strtoul(person->color.c_str() + 1, nullptr, 16));

            std::string schedule = chore.time.empty() ? "All day" : "Due " + chore.time;
            if (chore.completed) schedule = "Done";
            else if (chore.repeat == "once" && chore.date < state.today) schedule = "Overdue • " + chore.date;

            state.cards.push_back({
                title_lines(*fonts, *body, chore.title, text_width),
                fonts->ellipsize(*body, assigned ? std::string_view(person->name) : "Unassigned", text_width),
                fonts->ellipsize(*detail, schedule, text_width),
                static_cast<uint8_t>(color >> 16), static_cast<uint8_t>(color >> 8),
                static_cast<uint8_t>(color), chore.completed
            });
        }
    }

    for (const auto& card : state.cards) {
        SDL_SetRenderDrawColor(renderer, card.red, card.green, card.blue, card.completed ? 90 : 230);
        const SDL_FRect bar{float(x), float(y + px(2)), float(px(3)), float(card_height - px(10))};
        SDL_RenderFillRect(renderer, &bar);

        int text_y = y;
        for (const auto& line : card.title) {
            fonts->draw_text(text_x, text_y, *body, line, card.completed ? 130 : 245,
                             card.completed ? 155 : 250, card.completed ? 150 : 255, 255);
            text_y += body_step;
        }
        fonts->draw_text(text_x, text_y, *body, card.name, card.red, card.green, card.blue, 255);
        text_y += body_step;
        fonts->draw_text(text_x, text_y, *detail, card.schedule,
                         card.completed ? 130 : 180, card.completed ? 170 : 190, 185, 255);
        y += card_height;
    }
    if (pages > 1) {
        const auto footer = "Page " + std::to_string(page + 1) + " / " + std::to_string(pages);
        fonts->draw_text(x, footer_y, *detail, fonts->ellipsize(*detail, footer, available),
                         120, 145, 170, 255);
    }
}
