#include <curses.h>
#include <locale.h>

#include <array>
#include <deque>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMaxLogLines = 32;

std::string printable_ascii(wint_t key) {
    if (key >= 32 && key <= 126) {
        return std::string(1, static_cast<char>(key));
    }
    switch (key) {
        case '\n':
            return "\\n";
        case '\r':
            return "\\r";
        case '\t':
            return "\\t";
        case 27:
            return "ESC";
        default:
            return "";
    }
}

std::string hex_code(unsigned int value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << value;
    return out.str();
}

std::string special_key_name(wint_t key) {
    switch (key) {
        case KEY_LEFT:
            return "KEY_LEFT";
        case KEY_RIGHT:
            return "KEY_RIGHT";
        case KEY_UP:
            return "KEY_UP";
        case KEY_DOWN:
            return "KEY_DOWN";
        case KEY_SLEFT:
            return "KEY_SLEFT";
        case KEY_SRIGHT:
            return "KEY_SRIGHT";
        case KEY_SR:
            return "KEY_SR";
        case KEY_SF:
            return "KEY_SF";
        case KEY_BTAB:
            return "KEY_BTAB";
        case KEY_PPAGE:
            return "KEY_PPAGE";
        case KEY_NPAGE:
            return "KEY_NPAGE";
        case KEY_BACKSPACE:
            return "KEY_BACKSPACE";
        case KEY_MOUSE:
            return "KEY_MOUSE";
        default:
            return "KEY_" + std::to_string(static_cast<int>(key));
    }
}

std::string mouse_state(mmask_t bstate) {
    struct FlagName {
        mmask_t flag;
        const char *name;
    };
    static constexpr std::array<FlagName, 9> flags = {{
        {BUTTON1_PRESSED, "BUTTON1_PRESSED"},
        {BUTTON1_RELEASED, "BUTTON1_RELEASED"},
        {BUTTON1_CLICKED, "BUTTON1_CLICKED"},
        {BUTTON1_DOUBLE_CLICKED, "BUTTON1_DOUBLE_CLICKED"},
        {BUTTON1_TRIPLE_CLICKED, "BUTTON1_TRIPLE_CLICKED"},
        {BUTTON_SHIFT, "BUTTON_SHIFT"},
        {BUTTON_CTRL, "BUTTON_CTRL"},
        {BUTTON_ALT, "BUTTON_ALT"},
        {REPORT_MOUSE_POSITION, "REPORT_MOUSE_POSITION"},
    }};

    std::ostringstream out;
    bool first = true;
    for (const FlagName &entry : flags) {
        if ((bstate & entry.flag) == 0) {
            continue;
        }
        if (!first) {
            out << " | ";
        }
        out << entry.name;
        first = false;
    }
    if (first) {
        out << hex_code(static_cast<unsigned int>(bstate));
    }
    return out.str();
}

std::string describe_sequence(const std::vector<wint_t> &sequence) {
    std::ostringstream out;
    out << "escape sequence:";
    for (wint_t key : sequence) {
        out << ' ';
        std::string ascii = printable_ascii(key);
        if (!ascii.empty()) {
            out << ascii;
        } else {
            out << hex_code(static_cast<unsigned int>(key));
        }
    }
    return out.str();
}

void append_log(std::deque<std::string> &log, std::string line) {
    log.push_front(std::move(line));
    while (log.size() > kMaxLogLines) {
        log.pop_back();
    }
}

void draw(const std::deque<std::string> &log) {
    erase();
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    mvprintw(0, 0, "input_diag: Esc quits. Events shown newest first.");
    mvprintw(1, 0, "Terminal reports: shifted arrows may appear as KEY_SLEFT/KEY_SR or as raw ESC sequences.");
    int line = 3;
    for (const std::string &entry : log) {
        if (line >= rows) {
            break;
        }
        mvaddnstr(line, 0, entry.c_str(), cols - 1);
        ++line;
    }
    refresh();
}

}  // namespace

int main() {
    setlocale(LC_ALL, "");

    initscr();
    set_escdelay(25);
    raw();
    noecho();
    keypad(stdscr, TRUE);
    timeout(-1);
    mouseinterval(150);
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED, nullptr);

    std::deque<std::string> log;
    append_log(log, "Ready.");
    draw(log);

    bool running = true;
    while (running) {
        wint_t key = 0;
        int result = get_wch(&key);
        bool is_special = result == KEY_CODE_YES;
        if (result == ERR) {
            continue;
        }

        if (is_special && key == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                append_log(
                    log,
                    "mouse y=" + std::to_string(event.y) + " x=" + std::to_string(event.x) +
                        " bstate=" + mouse_state(event.bstate));
            } else {
                append_log(log, "mouse event read failed");
            }
            draw(log);
            continue;
        }

        if (!is_special && key == 27) {
            std::vector<wint_t> sequence;
            timeout(0);
            for (;;) {
                wint_t extra = 0;
                int extra_result = get_wch(&extra);
                if (extra_result == ERR) {
                    break;
                }
                sequence.push_back(extra);
            }
            timeout(-1);

            if (sequence.empty()) {
                running = false;
                break;
            }

            append_log(log, describe_sequence(sequence));
            draw(log);
            continue;
        }

        std::ostringstream out;
        if (is_special) {
            out << "special " << special_key_name(key) << " code=" << static_cast<int>(key);
        } else {
            out << "key code=" << static_cast<int>(key);
            std::string ascii = printable_ascii(key);
            if (!ascii.empty()) {
                out << " ascii=" << ascii;
            } else {
                out << " wchar=" << hex_code(static_cast<unsigned int>(key));
            }
        }
        append_log(log, out.str());
        draw(log);
    }

    keypad(stdscr, FALSE);
    noraw();
    nocbreak();
    echo();
    endwin();
    return 0;
}
