#include "tui.h"
#include "config_editor.h"
#include "config_text.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

class Terminal {
public:
    Terminal() {
        if (tcgetattr(STDIN_FILENO, &original) != 0) return;
        auto raw = original;
        // Handle Ctrl+C as a key so stack unwinding always restores the terminal.
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_iflag &= ~IXON;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        active = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
        if (active) std::fputs("\033[?1049h\033[?25l\033[0m\033[H\033[2J", stdout);
    }

    ~Terminal() {
        if (!active) return;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        std::fputs("\033[0m\033[?25h\033[?1049l", stdout);
        std::fflush(stdout);
    }

    bool active = false;
private:
    termios original{};
};

enum Key { Up = 256, Down, Left, Right, PageUp, PageDown };

int read_key() {
    unsigned char ch;
    if (read(STDIN_FILENO, &ch, 1) != 1) return -1;
    if (ch != 27) return ch;
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    if (poll(&fd, 1, 35) < 1 || read(STDIN_FILENO, &ch, 1) != 1 || (ch != '[' && ch != 'O')) return 27;
    if (poll(&fd, 1, 35) < 1 || read(STDIN_FILENO, &ch, 1) != 1) return 27;
    switch (ch) {
        case 'A': return Up;
        case 'B': return Down;
        case 'C': return Right;
        case 'D': return Left;
        case '5':
        case '6':
            if (poll(&fd, 1, 35) > 0) {
                unsigned char suffix;
                if (read(STDIN_FILENO, &suffix, 1) == 1 && suffix == '~') return ch == '5' ? PageUp : PageDown;
            }
            break;
    }
    return 27;
}

void row(std::string_view text, size_t width, const char* style = "") {
    std::fputs(style, stdout);
    size_t used = 0;
    for (unsigned char c : text) {
        if (used++ >= width) break;
        // Render controls and untrusted terminal sequences as inert text.
        std::fputc(c < 32 || c >= 127 ? '?' : c, stdout);
    }
    std::fputs("\033[0m\033[K\n", stdout);
}

} // namespace

void config_wizard(const std::string& path) {
    Terminal terminal;
    if (!terminal.active) {
        std::fputs("The configuration editor requires an interactive terminal.\n", stderr);
        return;
    }

    auto entries = config_editor::fields(g_cfg);
    if (entries.empty()) return;
    const auto defaults = config_editor::fields(Config{});
    const auto original_revision = config_editor::revision(g_cfg);
    std::map<std::string, std::string> edits;
    std::optional<std::string> input;
    std::string message;
    size_t selection = 0, first = 0;
    const auto change = [&](const config_editor::FieldView& field, const std::string& value) {
        edits.erase("clear." + field.id);
        if ((!field.secret && value == field.value) || (field.secret && value.empty())) edits.erase(field.id);
        else edits[field.id] = value;
    };

    while (true) {
        winsize size{};
        (void)ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
        const size_t width = size.ws_col > 1 ? size.ws_col - 1 : 79;
        const size_t visible = size.ws_row > 9 ? size.ws_row - 9 : 1;
        if (selection < first) first = selection;
        if (selection >= first + visible) first = selection - visible + 1;

        const auto& chosen = entries[selection];
        std::fputs("\033[H\033[2J", stdout);
        row("piTrove " VERSION " configuration", width, "\033[1m");
        row(chosen.group + "  |  " + std::to_string(selection + 1) + "/" + std::to_string(entries.size()), width);
        row("", width);
        for (size_t index = first; index < std::min(first + visible, entries.size()); ++index) {
            const auto& field = entries[index];
            auto value = field.secret ? (field.configured ? "[configured]" : "[empty]") : field.value;
            const auto edited = edits.find(field.id);
            const bool cleared = edits.contains("clear." + field.id);
            if (cleared) value = "[clear on save]";
            else if (edited != edits.end()) value = field.secret ? "[new value]" : edited->second;
            const auto label = (index == selection ? "> " : "  ") + field.label;
            row(label + (cleared || edited != edits.end() ? " *  " : "    ") + value,
                width, index == selection ? "\033[7m" : "");
        }
        row("", width);
        row(chosen.help, width);
        if (input) {
            row("Value: " + (chosen.secret ? std::string(std::min(input->size(), width), '*') : *input), width);
            row("Enter sets value   Esc cancels", width);
        } else {
            row("Up/Down   PgUp/PgDn   Left/Right section   Enter edit   Space next value", width);
            row("S save   Q quit   R reset section   X clear secret", width);
        }
        row(message, width);
        std::fflush(stdout);

        const int key = read_key();
        if (key < 0 || key == 3 || key == 4) return;
        message.clear();
        if (input) {
            if (key == 27) input.reset();
            else if (key == '\r' || key == '\n') {
                change(chosen, *input);
                input.reset();
            } else if ((key == 8 || key == 127) && !input->empty()) {
                do {
                    const auto last = static_cast<unsigned char>(input->back());
                    input->pop_back();
                    if ((last & 0xc0) != 0x80) break;
                } while (!input->empty());
            } else if (key >= 32 && key < 256 && input->size() < 32768) {
                input->push_back(static_cast<char>(key));
            }
            continue;
        }
        if (key == 'q' || key == 'Q') return;
        if (key == Up && selection > 0) --selection;
        if (key == Down && selection + 1 < entries.size()) ++selection;
        if (key == PageUp) selection -= std::min(selection, visible);
        if (key == PageDown) selection = std::min(selection + visible, entries.size() - 1);
        if (key == Left) {
            if (selection > 0) --selection;
            while (selection > 0 && entries[selection - 1].group == entries[selection].group) --selection;
        }
        if (key == Right) {
            while (selection + 1 < entries.size() && entries[selection].group == chosen.group) {
                if (entries[selection + 1].group != chosen.group) { ++selection; break; }
                ++selection;
            }
        }
        if ((key == 'x' || key == 'X') && chosen.secret) {
            edits.erase(chosen.id);
            if (chosen.configured) edits["clear." + chosen.id] = "1";
        }
        if (key == 'r' || key == 'R') {
            for (size_t index = 0; index < entries.size(); ++index) {
                const auto& field = entries[index];
                if (field.group == chosen.group && !field.secret) change(field, defaults[index].value);
            }
            message = "Defaults staged for this section. Save to apply.";
        }
        if (key == '\r' || key == '\n' || key == ' ') {
            const auto found = edits.find(chosen.id);
            const std::string value = found == edits.end() ? chosen.value : found->second;
            if (chosen.type == "boolean") change(chosen, value == "true" ? "false" : "true");
            else if (key == ' ' && !chosen.choices.empty()) {
                const auto old = std::find(chosen.choices.begin(), chosen.choices.end(), value);
                const auto next = old == chosen.choices.end() ? old : std::next(old);
                change(chosen, next == chosen.choices.end() ? chosen.choices.front() : *next);
            } else if (key != ' ') input = chosen.secret ? "" : value;
        }
        if (key == 's' || key == 'S') {
            if (edits.empty()) return;
            std::lock_guard lock(g_config_mtx);
            edits["revision"] = original_revision;
            const auto result = config_editor::apply(g_cfg, edits, path);
            edits.erase("revision");
            if (result.status == 200) {
                g_config_changed.store(!result.changed.empty());
                return;
            }
            const auto quote = result.body.find(':');
            message = quote != result.body.npos && result.body.ends_with('}')
                ? config_text::decode(std::string_view(result.body).substr(quote + 1, result.body.size() - quote - 2)).value_or(result.body)
                : result.body;
        }
    }
}
