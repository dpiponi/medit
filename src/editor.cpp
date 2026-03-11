#include "config.hpp"
#include "editor_core.hpp"
#include "keybindings.hpp"
#include "services.hpp"
#include "theme.hpp"

#include <clocale>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <ncursesw/curses.h>
#include <optional>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

enum class Mode {
    Normal,
    Insert,
    Visual,
    VisualLine,
    Command,
    Search,
};

enum class PendingMotion {
    None,
    FindForward,
    FindBackward,
    TillForward,
    TillBackward,
};

struct EditorState {
    EditorCore core;
    EditorRuntime runtime;
    KeyBindings keybindings;
    Theme theme = load_embedded_theme();
    std::size_t row_offset = 0;
    std::size_t col_offset = 0;
    bool should_quit = false;
    Mode mode = Mode::Normal;
    std::u32string command_buffer;
    std::u32string search_buffer;
    std::string status_message = "NORMAL";
    std::vector<std::string> pending_tokens;
    PendingMotion pending_motion = PendingMotion::None;
    std::u32string active_search_pattern;
    std::vector<Range> search_matches;
    std::optional<std::size_t> current_search_match_index;
    Position search_origin;
    bool search_pattern_valid = true;
    std::string compiled_search_pattern_utf8;
    std::unique_ptr<std::regex> compiled_search_regex;
    std::size_t search_matches_version = 0;
};

std::wstring u32_to_wstring(const std::u32string &text) {
    std::wstring result;
    result.reserve(text.size());
    for (char32_t codepoint : text) {
        result.push_back(static_cast<wchar_t>(codepoint));
    }
    return result;
}

enum class ThemeSlot : short {
    Default = 1,
    LineNumber = 2,
    CursorLine = 3,
    CursorLineNumber = 4,
    StatusBar = 5,
    MessageBar = 6,
    CommandLine = 7,
    Selection = 8,
    SearchMatch = 9,
    SearchMatchCurrent = 10,
    SyntaxKeyword = 11,
    SyntaxString = 12,
    SyntaxComment = 13,
    DiagnosticError = 14,
    DiagnosticWarning = 15,
};

ThemeSlot theme_slot(StyleRole role);

int codepoint_width(char32_t codepoint) {
    wchar_t wide = static_cast<wchar_t>(codepoint);
    int width = wcwidth(wide);
    return width > 0 ? width : 1;
}

std::string mode_name(Mode mode) {
    switch (mode) {
        case Mode::Normal:
            return "NORMAL";
        case Mode::Insert:
            return "INSERT";
        case Mode::Visual:
            return "VISUAL";
        case Mode::VisualLine:
            return "VISUAL LINE";
        case Mode::Command:
            return "COMMAND";
        case Mode::Search:
            return "SEARCH";
    }
    return "UNKNOWN";
}

void set_status(EditorState &state, const std::string &message) {
    state.status_message = message;
}

void initialize_locale() {
    setlocale(LC_ALL, "");
}

void setup_terminal(const Theme &theme) {
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    timeout(-1);
    mouseinterval(150);
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED, nullptr);
    start_color();
    use_default_colors();
    for (int role_index = 0; role_index <= static_cast<int>(StyleRole::DiagnosticWarning); ++role_index) {
        StyleRole role = static_cast<StyleRole>(role_index);
        TextStyle style = theme_style(theme, role);
        init_pair(static_cast<short>(theme_slot(role)), style.foreground, style.background);
    }
    curs_set(1);
}

void teardown_terminal() {
    endwin();
}

void enter_normal_mode(EditorState &state) {
    if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
        state.core.clear_selection();
    }
    state.mode = Mode::Normal;
    state.command_buffer.clear();
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.search_buffer.clear();
    Position cursor = state.core.cursor();
    std::size_t length = state.core.line_length(cursor.row);
    if (cursor.column > 0 && cursor.column == length) {
        state.core.set_cursor({cursor.row, cursor.column - 1});
    }
    set_status(state, mode_name(state.mode));
}

void enter_insert_mode(EditorState &state) {
    state.core.clear_selection();
    state.mode = Mode::Insert;
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.search_buffer.clear();
    set_status(state, mode_name(state.mode));
}

void enter_command_mode(EditorState &state) {
    state.core.clear_selection();
    state.mode = Mode::Command;
    state.command_buffer.clear();
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.search_buffer.clear();
    set_status(state, ":");
}

void enter_search_mode(EditorState &state) {
    state.core.clear_selection();
    state.mode = Mode::Search;
    state.search_buffer = state.active_search_pattern;
    state.search_origin = state.core.cursor();
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    set_status(state, "/");
}

bool can_quit_without_force(const EditorState &state) {
    return !state.core.is_dirty();
}

void quit_editor(EditorState &state) {
    state.should_quit = true;
}

void handle_write_command(EditorState &state, const std::string &argument) {
    if (argument.empty()) {
        if (state.core.save_current_file()) {
            set_status(state, "Wrote " + state.core.display_file_name());
        } else {
            set_status(state, state.core.file_path() ? "Write failed" : "No file name");
        }
        return;
    }
    if (state.core.save_current_file_as(argument)) {
        set_status(state, "Wrote " + argument);
    } else {
        set_status(state, "Write failed");
    }
}

void handle_edit_command(EditorState &state, const std::string &argument) {
    if (argument.empty()) {
        set_status(state, "No file name");
        return;
    }
    if (state.core.load_file(argument)) {
        state.row_offset = 0;
        state.col_offset = 0;
        set_status(state, "Opened " + argument);
    } else {
        set_status(state, "Could not open file");
    }
}

void handle_quit_command(EditorState &state, bool force) {
    if (!force && !can_quit_without_force(state)) {
        set_status(state, "Unsaved changes; use :q! to quit");
        return;
    }
    quit_editor(state);
}

void handle_write_quit_command(EditorState &state, const std::string &argument) {
    if (argument.empty()) {
        if (!state.core.save_current_file()) {
            set_status(state, state.core.file_path() ? "Write failed" : "No file name");
            return;
        }
        quit_editor(state);
        return;
    }
    if (!state.core.save_current_file_as(argument)) {
        set_status(state, "Write failed");
        return;
    }
    quit_editor(state);
}

void execute_command(EditorState &state) {
    std::string command = u32_to_utf8(state.command_buffer);
    std::istringstream parser(command);
    std::string verb;
    parser >> verb;

    std::string argument;
    std::getline(parser, argument);
    if (!argument.empty() && argument.front() == ' ') {
        argument.erase(argument.begin());
    }

    if (verb.empty()) {
        enter_normal_mode(state);
        return;
    }
    if (verb == "w") {
        handle_write_command(state, argument);
    } else if (verb == "q") {
        handle_quit_command(state, false);
    } else if (verb == "q!") {
        handle_quit_command(state, true);
    } else if (verb == "wq" || verb == "x") {
        handle_write_quit_command(state, argument);
    } else if (verb == "e") {
        handle_edit_command(state, argument);
    } else {
        set_status(state, "Unknown command: " + verb);
    }

    if (!state.should_quit) {
        state.mode = Mode::Normal;
        state.command_buffer.clear();
        state.pending_tokens.clear();
    }
}

void set_search_status(EditorState &state, const std::string &suffix = "") {
    std::string prompt = "/" + u32_to_utf8(state.search_buffer);
    if (!suffix.empty()) {
        prompt += "  " + suffix;
    }
    set_status(state, prompt);
}

std::size_t column_for_utf8_offset_in_line(const std::u32string &line, std::size_t offset) {
    std::size_t remaining = offset;
    for (std::size_t column = 0; column < line.size(); ++column) {
        std::size_t width = u32_to_utf8(std::u32string(1, line[column])).size();
        if (remaining < width) {
            return column;
        }
        remaining -= width;
    }
    return line.size();
}

void rebuild_search_matches(EditorState &state) {
    state.search_matches.clear();
    state.current_search_match_index.reset();
    state.search_pattern_valid = true;
    state.search_matches_version = state.core.document_version();

    if (state.active_search_pattern.empty()) {
        state.compiled_search_pattern_utf8.clear();
        state.compiled_search_regex.reset();
        return;
    }

    try {
        state.compiled_search_pattern_utf8 = u32_to_utf8(state.active_search_pattern);
        state.compiled_search_regex =
            std::make_unique<std::regex>(state.compiled_search_pattern_utf8, std::regex::ECMAScript | std::regex::optimize);

        for (std::size_t row = 0; row < state.core.line_count(); ++row) {
            const std::u32string &line = state.core.lines()[row];
            std::string line_utf8 = u32_to_utf8(line);
            for (std::sregex_iterator it(line_utf8.begin(), line_utf8.end(), *state.compiled_search_regex), end; it != end; ++it) {
                if (it->length() == 0) {
                    continue;
                }
                std::size_t start_offset = static_cast<std::size_t>(it->position());
                std::size_t end_offset = start_offset + static_cast<std::size_t>(it->length());
                state.search_matches.push_back(
                    {{row, column_for_utf8_offset_in_line(line, start_offset)},
                     {row, column_for_utf8_offset_in_line(line, end_offset)}});
            }
        }
    } catch (const std::regex_error &) {
        state.search_pattern_valid = false;
        state.compiled_search_regex.reset();
    }
}

void refresh_search_matches(EditorState &state, bool move_to_best_match) {
    if (state.search_matches_version != state.core.document_version() ||
        state.compiled_search_pattern_utf8 != u32_to_utf8(state.active_search_pattern)) {
        rebuild_search_matches(state);
    }

    if (!state.search_pattern_valid || state.search_matches.empty()) {
        if (move_to_best_match) {
            state.current_search_match_index.reset();
        }
        return;
    }

    Position anchor = move_to_best_match ? state.search_origin : state.core.cursor();
    std::size_t best_index = 0;
    bool found_best = false;
    for (std::size_t index = 0; index < state.search_matches.size(); ++index) {
        if (!position_less_than(state.search_matches[index].start, anchor)) {
            best_index = index;
            found_best = true;
            break;
        }
    }
    state.current_search_match_index = found_best ? best_index : 0;
    if (move_to_best_match) {
        state.core.set_cursor(state.search_matches[*state.current_search_match_index].start);
    }
}

void navigate_search_match(EditorState &state, bool forward) {
    if (state.search_matches.empty()) {
        set_status(state, state.search_pattern_valid ? "No search matches" : "invalid regex");
        return;
    }

    std::size_t index = state.current_search_match_index.value_or(0);
    if (forward) {
        index = (index + 1) % state.search_matches.size();
    } else {
        index = index == 0 ? state.search_matches.size() - 1 : index - 1;
    }
    state.current_search_match_index = index;
    state.core.set_cursor(state.search_matches[index].start);
    set_status(state, forward ? "Next match" : "Previous match");
}

std::size_t display_width_until(const std::u32string &line, std::size_t limit) {
    std::size_t width = 0;
    std::size_t capped = limit > line.size() ? line.size() : limit;
    for (std::size_t i = 0; i < capped; ++i) {
        width += static_cast<std::size_t>(codepoint_width(line[i]));
    }
    return width;
}

void ensure_horizontal_visibility(EditorState &state, int screen_cols) {
    Position cursor = state.core.cursor();
    const std::u32string &line = state.core.lines()[cursor.row];
    std::size_t cursor_x = display_width_until(line, cursor.column);
    std::size_t usable_cols = screen_cols > 0 ? static_cast<std::size_t>(screen_cols) : 1;

    while (state.col_offset > cursor_x) {
        --state.col_offset;
    }
    while (cursor_x >= state.col_offset + usable_cols) {
        ++state.col_offset;
    }
}

void ensure_vertical_visibility(EditorState &state, int screen_rows) {
    std::size_t usable_rows = screen_rows > 0 ? static_cast<std::size_t>(screen_rows) : 1;
    Position cursor = state.core.cursor();
    if (cursor.row < state.row_offset) {
        state.row_offset = cursor.row;
    }
    while (cursor.row >= state.row_offset + usable_rows) {
        ++state.row_offset;
    }
}

void ensure_cursor_visible(EditorState &state, int buffer_rows, int buffer_cols) {
    ensure_vertical_visibility(state, buffer_rows);
    ensure_horizontal_visibility(state, buffer_cols);
}

ThemeSlot theme_slot(StyleRole role) {
    switch (role) {
        case StyleRole::DefaultText:
            return ThemeSlot::Default;
        case StyleRole::LineNumber:
            return ThemeSlot::LineNumber;
        case StyleRole::CursorLine:
            return ThemeSlot::CursorLine;
        case StyleRole::CursorLineNumber:
            return ThemeSlot::CursorLineNumber;
        case StyleRole::StatusBar:
            return ThemeSlot::StatusBar;
        case StyleRole::MessageBar:
            return ThemeSlot::MessageBar;
        case StyleRole::CommandLine:
            return ThemeSlot::CommandLine;
        case StyleRole::Selection:
            return ThemeSlot::Selection;
        case StyleRole::SearchMatch:
            return ThemeSlot::SearchMatch;
        case StyleRole::SearchMatchCurrent:
            return ThemeSlot::SearchMatchCurrent;
        case StyleRole::SyntaxKeyword:
            return ThemeSlot::SyntaxKeyword;
        case StyleRole::SyntaxString:
            return ThemeSlot::SyntaxString;
        case StyleRole::SyntaxComment:
            return ThemeSlot::SyntaxComment;
        case StyleRole::DiagnosticError:
            return ThemeSlot::DiagnosticError;
        case StyleRole::DiagnosticWarning:
            return ThemeSlot::DiagnosticWarning;
    }
    return ThemeSlot::Default;
}

int curses_attributes(TextStyle style, StyleRole role) {
    int attrs = COLOR_PAIR(static_cast<short>(theme_slot(role)));
    if (style.bold) {
        attrs |= A_BOLD;
    }
    if (style.underline) {
        attrs |= A_UNDERLINE;
    }
    if (style.reverse) {
        attrs |= A_REVERSE;
    }
    return attrs;
}

StyleRole resolve_style_role(
    Position position,
    const std::vector<HighlightSpan> &spans,
    StyleRole base_role) {
    StyleRole resolved_role = base_role;
    int resolved_priority = -1000000;
    for (const HighlightSpan &span : spans) {
        if (!range_contains(span.range, position)) {
            continue;
        }
        if (span.priority >= resolved_priority) {
            resolved_priority = span.priority;
            resolved_role = span.role;
        }
    }
    return resolved_role;
}

std::vector<HighlightSpan> collect_line_highlights(const EditorState &state, std::size_t row) {
    std::vector<HighlightSpan> spans;
    Range entire_line = state.core.line_range(row);
    if (state.core.cursor().row == row) {
        spans.push_back({entire_line, StyleRole::CursorLine, 10});
    }
    std::optional<Range> selection = state.core.selection_range();
    if (selection && row >= selection->start.row && row <= selection->end.row) {
        Range line_selection{
            {row, row == selection->start.row ? selection->start.column : 0},
            {row, row == selection->end.row ? selection->end.column : state.core.line_length(row)}};
        if (!positions_equal(line_selection.start, line_selection.end)) {
            spans.push_back({line_selection, StyleRole::Selection, 100});
        }
    }
    for (std::size_t index = 0; index < state.search_matches.size(); ++index) {
        const Range &match = state.search_matches[index];
        if (row < match.start.row || row > match.end.row) {
            continue;
        }
        Range line_match{
            {row, row == match.start.row ? match.start.column : 0},
            {row, row == match.end.row ? match.end.column : state.core.line_length(row)}};
        if (positions_equal(line_match.start, line_match.end)) {
            continue;
        }
        StyleRole role =
            state.current_search_match_index && *state.current_search_match_index == index
                ? StyleRole::SearchMatchCurrent
                : StyleRole::SearchMatch;
        int priority = role == StyleRole::SearchMatchCurrent ? 95 : 90;
        spans.push_back({line_match, role, priority});
    }
    return spans;
}

void render_styled_glyph(const Theme &theme, int screen_row, int screen_col, char32_t codepoint, StyleRole role) {
    std::wstring glyph(1, static_cast<wchar_t>(codepoint));
    TextStyle style = theme_style(theme, role);
    attr_t attrs = static_cast<attr_t>(curses_attributes(style, role));
    mvaddnwstr(screen_row, screen_col, glyph.c_str(), 1);
    mvchgat(screen_row, screen_col, codepoint_width(codepoint), attrs, static_cast<short>(theme_slot(role)), nullptr);
}

void draw_styled_line(
    const Theme &theme,
    int screen_row,
    int start_col,
    const std::u32string &line,
    const std::vector<HighlightSpan> &spans,
    std::size_t row,
    std::size_t col_offset,
    int max_cols) {
    std::size_t skipped_width = 0;
    int screen_col = start_col;

    for (std::size_t column = 0; column < line.size(); ++column) {
        char32_t codepoint = line[column];
        int width = codepoint_width(codepoint);
        if (skipped_width + static_cast<std::size_t>(width) <= col_offset) {
            skipped_width += static_cast<std::size_t>(width);
            continue;
        }
        if (screen_col + width > start_col + max_cols) {
            break;
        }
        StyleRole role = resolve_style_role({row, column}, spans, StyleRole::DefaultText);
        render_styled_glyph(theme, screen_row, screen_col, codepoint, role);
        screen_col += width;
    }
}

void draw_line_number(const Theme &theme, int screen_row, std::size_t line_number, int line_number_width, StyleRole role) {
    TextStyle style = theme_style(theme, role);
    attron(curses_attributes(style, role));
    mvprintw(screen_row, 0, "%*zu ", line_number_width, line_number);
    attroff(curses_attributes(style, role));
}

void draw_buffer_rows(const EditorState &state, int buffer_rows, int buffer_cols, int line_number_width) {
    for (int screen_row = 0; screen_row < buffer_rows; ++screen_row) {
        std::size_t line_index = state.row_offset + static_cast<std::size_t>(screen_row);
        move(screen_row, 0);
        clrtoeol();
        if (line_index >= state.core.line_count()) {
            mvaddch(screen_row, 0, '~');
            continue;
        }
        StyleRole line_number_role =
            state.core.cursor().row == line_index ? StyleRole::CursorLineNumber : StyleRole::LineNumber;
        draw_line_number(state.theme, screen_row, line_index + 1, line_number_width, line_number_role);
        std::vector<HighlightSpan> spans = collect_line_highlights(state, line_index);
        draw_styled_line(
            state.theme,
            screen_row,
            line_number_width + 1,
            state.core.lines()[line_index],
            spans,
            line_index,
            state.col_offset,
            buffer_cols);
    }
}

std::string build_status_text(const EditorState &state) {
    Position cursor = state.core.cursor();
    std::ostringstream status;
    status << mode_name(state.mode) << "  " << state.core.display_file_name();
    if (state.core.is_dirty()) {
        status << " [+]";
    }
    status << "  " << (cursor.row + 1) << ":" << (cursor.column + 1);
    status << "  rev " << state.core.current_revision();
    return status.str();
}

void draw_status_bar(const EditorState &state, int screen_rows, int screen_cols) {
    TextStyle style = theme_style(state.theme, StyleRole::StatusBar);
    attron(curses_attributes(style, StyleRole::StatusBar));
    move(screen_rows - 2, 0);
    clrtoeol();
    std::string status = build_status_text(state);
    mvaddnstr(screen_rows - 2, 0, status.c_str(), screen_cols);
    attroff(curses_attributes(style, StyleRole::StatusBar));
}

void draw_message_bar(const EditorState &state, int screen_rows, int screen_cols) {
    move(screen_rows - 1, 0);
    clrtoeol();
    if (state.mode == Mode::Command) {
        TextStyle style = theme_style(state.theme, StyleRole::CommandLine);
        attron(curses_attributes(style, StyleRole::CommandLine));
        std::string command = ":" + u32_to_utf8(state.command_buffer);
        mvaddnstr(screen_rows - 1, 0, command.c_str(), screen_cols);
        attroff(curses_attributes(style, StyleRole::CommandLine));
        return;
    }
    if (state.mode == Mode::Search) {
        TextStyle style = theme_style(state.theme, StyleRole::CommandLine);
        attron(curses_attributes(style, StyleRole::CommandLine));
        std::string command = "/" + u32_to_utf8(state.search_buffer);
        mvaddnstr(screen_rows - 1, 0, command.c_str(), screen_cols);
        attroff(curses_attributes(style, StyleRole::CommandLine));
        return;
    }
    TextStyle style = theme_style(state.theme, StyleRole::MessageBar);
    attron(curses_attributes(style, StyleRole::MessageBar));
    mvaddnstr(screen_rows - 1, 0, state.status_message.c_str(), screen_cols);
    attroff(curses_attributes(style, StyleRole::MessageBar));
}

int line_number_width(const EditorState &state) {
    std::size_t line_count = state.core.line_count();
    int width = 1;
    while (line_count >= 10) {
        line_count /= 10;
        ++width;
    }
    return width;
}

std::pair<int, int> cursor_screen_position(const EditorState &state, int line_number_cols) {
    Position cursor = state.core.cursor();
    const std::u32string &line = state.core.lines()[cursor.row];
    std::size_t width = display_width_until(line, cursor.column);
    int screen_row = static_cast<int>(cursor.row - state.row_offset);
    int screen_col = static_cast<int>(width - state.col_offset) + line_number_cols + 1;
    return {screen_row, screen_col};
}

std::size_t column_for_display_width(const std::u32string &line, std::size_t target_width) {
    std::size_t width = 0;
    for (std::size_t column = 0; column < line.size(); ++column) {
        std::size_t next_width = width + static_cast<std::size_t>(codepoint_width(line[column]));
        if (target_width < next_width) {
            return column;
        }
        width = next_width;
    }
    return line.size();
}

std::optional<Position> buffer_position_from_screen_point(const EditorState &state, int screen_row, int screen_col) {
    int total_rows = 0;
    int total_cols = 0;
    getmaxyx(stdscr, total_rows, total_cols);
    (void)total_cols;

    int buffer_rows = total_rows - 2;
    if (screen_row < 0 || screen_row >= buffer_rows) {
        return std::nullopt;
    }

    std::size_t row = state.row_offset + static_cast<std::size_t>(screen_row);
    if (row >= state.core.line_count()) {
        return std::nullopt;
    }

    int gutter_start = line_number_width(state) + 1;
    std::size_t visual_column = state.col_offset;
    if (screen_col > gutter_start) {
        visual_column += static_cast<std::size_t>(screen_col - gutter_start);
    }

    const std::u32string &line = state.core.lines()[row];
    return Position{row, column_for_display_width(line, visual_column)};
}

void draw_editor(const EditorState &state) {
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);

    int buffer_rows = screen_rows - 2;
    int number_cols = line_number_width(state);
    int buffer_cols = screen_cols - number_cols - 1;

    erase();
    draw_buffer_rows(state, buffer_rows, buffer_cols, number_cols);
    draw_status_bar(state, screen_rows, screen_cols);
    draw_message_bar(state, screen_rows, screen_cols);

    auto [cursor_row, cursor_col] = cursor_screen_position(state, number_cols);
    move(cursor_row, cursor_col);
    refresh();
}

void append_after_cursor(EditorState &state) {
    Position cursor = state.core.cursor();
    std::size_t length = state.core.line_length(cursor.row);
    if (cursor.column < length) {
        state.core.set_cursor({cursor.row, cursor.column + 1});
    }
    enter_insert_mode(state);
}

int page_step() {
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);
    (void)screen_cols;
    int step = screen_rows - 2;
    return step > 0 ? step : 1;
}

int half_page_step() {
    int step = page_step() / 2;
    return step > 0 ? step : 1;
}

void page_up(EditorState &state) {
    state.core.move_by_lines(-page_step());
}

void page_down(EditorState &state) {
    state.core.move_by_lines(page_step());
}

void half_page_up(EditorState &state) {
    state.core.move_by_lines(-half_page_step());
}

void half_page_down(EditorState &state) {
    state.core.move_by_lines(half_page_step());
}

void enter_visual_mode(EditorState &state) {
    state.mode = Mode::Visual;
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.core.begin_selection(SelectionMode::Character);
    set_status(state, mode_name(state.mode));
}

void enter_visual_line_mode(EditorState &state) {
    state.mode = Mode::VisualLine;
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.core.begin_selection(SelectionMode::Line);
    set_status(state, mode_name(state.mode));
}

bool motion_is_character_based(PendingMotion motion) {
    return motion != PendingMotion::None;
}

bool execute_pending_motion(EditorState &state, char32_t target) {
    bool moved = false;
    switch (state.pending_motion) {
        case PendingMotion::FindForward:
            moved = state.core.move_to_character_forward(target, true);
            break;
        case PendingMotion::FindBackward:
            moved = state.core.move_to_character_backward(target, true);
            break;
        case PendingMotion::TillForward:
            moved = state.core.move_to_character_forward(target, false);
            break;
        case PendingMotion::TillBackward:
            moved = state.core.move_to_character_backward(target, false);
            break;
        case PendingMotion::None:
            return false;
    }
    state.pending_motion = PendingMotion::None;
    state.pending_tokens.clear();
    set_status(state, moved ? mode_name(state.mode) : "Target not found");
    return true;
}

std::optional<std::string> key_token(wint_t key, bool is_special) {
    if (is_special) {
        switch (key) {
            case KEY_LEFT:
                return "left";
            case KEY_RIGHT:
                return "right";
            case KEY_UP:
                return "up";
            case KEY_DOWN:
                return "down";
            case KEY_PPAGE:
                return "pageup";
            case KEY_NPAGE:
                return "pagedown";
            case KEY_BACKSPACE:
                return "backspace";
            default:
                return std::nullopt;
        }
    }

    if (key == 27) {
        return "esc";
    }
    if (key == '\n' || key == '\r' || key == KEY_ENTER) {
        return "enter";
    }
    if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
        return "backspace";
    }
    if (key >= 1 && key <= 26) {
        std::string token = "ctrl-";
        token.push_back(static_cast<char>('a' + key - 1));
        return token;
    }

    return u32_to_utf8(std::u32string(1, static_cast<char32_t>(key)));
}

std::string mode_key(const EditorState &state) {
    switch (state.mode) {
        case Mode::Normal:
            return "normal";
        case Mode::Insert:
            return "insert";
        case Mode::Visual:
        case Mode::VisualLine:
            return "visual";
        case Mode::Command:
            return "command";
        case Mode::Search:
            return "search";
    }
    return "normal";
}

bool is_printable_input(wint_t key, bool is_special) {
    if (is_special) {
        return false;
    }
    if (key == 27 || key == '\n' || key == '\r' || key == KEY_ENTER) {
        return false;
    }
    if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
        return false;
    }
    if (key >= 1 && key <= 26) {
        return false;
    }
    return key >= 32 || key == '\t';
}

void execute_action(EditorState &state, EditorAction action, wint_t key) {
    switch (action) {
        case EditorAction::MoveLeft:
            state.core.move_left();
            break;
        case EditorAction::MoveRight:
            state.core.move_right();
            break;
        case EditorAction::MoveUp:
            state.core.move_up();
            break;
        case EditorAction::MoveDown:
            state.core.move_down();
            break;
        case EditorAction::MoveLineStart:
            state.core.move_line_start();
            break;
        case EditorAction::MoveLineEnd:
            state.core.move_line_end();
            break;
        case EditorAction::FindForward:
            state.pending_motion = PendingMotion::FindForward;
            set_status(state, "f");
            break;
        case EditorAction::FindBackward:
            state.pending_motion = PendingMotion::FindBackward;
            set_status(state, "F");
            break;
        case EditorAction::TillForward:
            state.pending_motion = PendingMotion::TillForward;
            set_status(state, "t");
            break;
        case EditorAction::TillBackward:
            state.pending_motion = PendingMotion::TillBackward;
            set_status(state, "T");
            break;
        case EditorAction::EnterInsertMode:
            enter_insert_mode(state);
            break;
        case EditorAction::AppendAfterCursor:
            append_after_cursor(state);
            break;
        case EditorAction::EnterVisualMode:
            enter_visual_mode(state);
            break;
        case EditorAction::EnterVisualLineMode:
            enter_visual_line_mode(state);
            break;
        case EditorAction::AppendLineEndInsert: {
            Position cursor = state.core.cursor();
            state.core.set_cursor({cursor.row, state.core.line_length(cursor.row)});
            enter_insert_mode(state);
            break;
        }
        case EditorAction::InsertLineStartInsert: {
            Position cursor = state.core.cursor();
            state.core.set_cursor({cursor.row, 0});
            enter_insert_mode(state);
            break;
        }
        case EditorAction::OpenLineBelow:
            state.core.open_line_below();
            state.mode = Mode::Insert;
            set_status(state, mode_name(state.mode));
            break;
        case EditorAction::OpenLineAbove:
            state.core.open_line_above();
            state.mode = Mode::Insert;
            set_status(state, mode_name(state.mode));
            break;
        case EditorAction::DeleteChar:
            state.core.delete_character_under_cursor();
            set_status(state, "Deleted character");
            break;
        case EditorAction::Undo:
            set_status(state, state.core.undo() ? "Undid change" : "Nothing to undo");
            break;
        case EditorAction::Redo:
            set_status(state, state.core.redo() ? "Redid change" : "Nothing to redo");
            break;
        case EditorAction::PasteAfter:
            set_status(state, state.core.paste_after_cursor() ? "Pasted" : "Yank buffer empty");
            break;
        case EditorAction::PasteBefore:
            set_status(state, state.core.paste_before_cursor() ? "Pasted" : "Yank buffer empty");
            break;
        case EditorAction::GotoTop:
            state.core.move_to_first_line();
            state.core.move_line_start();
            set_status(state, "Top of file");
            break;
        case EditorAction::GotoBottom:
            state.core.move_to_last_line();
            state.core.move_line_start();
            set_status(state, "Bottom of file");
            break;
        case EditorAction::EnterCommandMode:
            enter_command_mode(state);
            break;
        case EditorAction::EnterSearchMode:
            enter_search_mode(state);
            refresh_search_matches(state, true);
            set_search_status(state);
            break;
        case EditorAction::DeleteLine:
            state.core.delete_current_line();
            set_status(state, "Deleted line");
            break;
        case EditorAction::HalfPageDown:
            half_page_down(state);
            break;
        case EditorAction::HalfPageUp:
            half_page_up(state);
            break;
        case EditorAction::PageUp:
            page_up(state);
            break;
        case EditorAction::PageDown:
            page_down(state);
            break;
        case EditorAction::EnterNormalMode:
            enter_normal_mode(state);
            break;
        case EditorAction::InsertNewline:
            state.core.insert_newline();
            break;
        case EditorAction::Backspace:
            state.core.backspace_character();
            break;
        case EditorAction::CommandExecute:
            execute_command(state);
            break;
        case EditorAction::CommandBackspace:
            if (!state.command_buffer.empty()) {
                state.command_buffer.pop_back();
            }
            break;
        case EditorAction::SelfInsert:
            state.core.insert_codepoint(static_cast<char32_t>(key));
            break;
        case EditorAction::CommandInsert:
            state.command_buffer.push_back(static_cast<char32_t>(key));
            break;
        case EditorAction::SearchExecute:
            state.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            if (!state.search_pattern_valid) {
                set_search_status(state, "invalid regex");
                break;
            }
            state.mode = Mode::Normal;
            set_status(state, state.search_matches.empty() ? "No search matches" : "Search applied");
            break;
        case EditorAction::SearchBackspace:
            if (!state.search_buffer.empty()) {
                state.search_buffer.pop_back();
            }
            state.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            set_search_status(state, state.search_pattern_valid ? "" : "invalid regex");
            break;
        case EditorAction::SearchInsert:
            state.search_buffer.push_back(static_cast<char32_t>(key));
            state.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            set_search_status(state, state.search_pattern_valid ? "" : "invalid regex");
            break;
        case EditorAction::SearchNext:
            refresh_search_matches(state, false);
            navigate_search_match(state, true);
            break;
        case EditorAction::SearchPrevious:
            refresh_search_matches(state, false);
            navigate_search_match(state, false);
            break;
        case EditorAction::DeleteSelection:
            {
            SelectionMode selection_mode = state.core.selection_mode();
            if (state.core.delete_selection()) {
                state.mode = Mode::Normal;
                set_status(
                    state,
                    selection_mode == SelectionMode::Line ? "Deleted lines" : "Deleted selection");
            } else {
                set_status(state, "No selection");
            }
            break;
            }
        case EditorAction::YankSelection:
            if (state.core.yank_selection()) {
                SelectionMode selection_mode = state.core.selection_mode();
                state.core.clear_selection();
                state.mode = Mode::Normal;
                set_status(state, selection_mode == SelectionMode::Line ? "Yanked lines" : "Yanked selection");
            } else {
                set_status(state, "No selection");
            }
            break;
        case EditorAction::ChangeSelection:
            {
            if (state.core.delete_selection()) {
                enter_insert_mode(state);
            } else {
                set_status(state, "No selection");
            }
            break;
            }
        case EditorAction::ReplaceSelectionWithYank:
            {
            SelectionMode selection_mode = state.core.selection_mode();
            if (state.core.replace_selection_with_yank()) {
                state.mode = Mode::Normal;
                set_status(
                    state,
                    selection_mode == SelectionMode::Line ? "Replaced lines" : "Replaced selection");
            } else {
                set_status(state, "Yank buffer empty");
            }
            break;
            }
        case EditorAction::SelectInnerWord:
            if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
                std::optional<Range> range = state.core.inner_word_range();
                if (range && state.core.extend_selection_to_range(*range)) {
                    state.mode = Mode::Visual;
                    set_status(state, "VISUAL");
                } else {
                    set_status(state, "No word");
                }
            }
            break;
        case EditorAction::SelectAroundWord:
            if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
                std::optional<Range> range = state.core.a_word_range();
                if (range && state.core.extend_selection_to_range(*range)) {
                    state.mode = Mode::Visual;
                    set_status(state, "VISUAL");
                } else {
                    set_status(state, "No word");
                }
            }
            break;
    }
}

void handle_keymap_input(EditorState &state, wint_t key, bool is_special) {
    std::optional<std::string> token = key_token(key, is_special);
    if (!token) {
        return;
    }
    KeyDispatch dispatch = dispatch_key_sequence(
        state.keybindings,
        mode_key(state),
        state.pending_tokens,
        *token,
        is_printable_input(key, is_special));
    if (dispatch.waiting_for_more) {
        set_status(state, u32_to_utf8(utf8_to_u32(*token)));
        return;
    }
    if (dispatch.action) {
        execute_action(state, *dispatch.action, key);
    }
}

void handle_mouse_input(EditorState &state) {
    MEVENT event;
    if (getmouse(&event) != OK) {
        return;
    }
    mmask_t accepted = BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED;
    if ((event.bstate & accepted) == 0) {
        return;
    }

    std::optional<Position> clicked = buffer_position_from_screen_point(state, event.y, event.x);
    if (!clicked) {
        return;
    }

    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.core.set_cursor(*clicked);
    if (state.mode == Mode::Command) {
        set_status(state, ":");
        return;
    }
    if (state.mode == Mode::Search) {
        set_search_status(state, state.search_pattern_valid ? "" : "invalid regex");
        return;
    }
    set_status(state, mode_name(state.mode));
}

void handle_input(EditorState &state) {
    wint_t key = 0;
    int result = get_wch(&key);
    bool is_special = result == KEY_CODE_YES;
    if (result == ERR) {
        return;
    }

    if (is_special && key == KEY_MOUSE) {
        handle_mouse_input(state);
        return;
    }

    if (motion_is_character_based(state.pending_motion)) {
        if (!is_special && key == 27) {
            state.pending_motion = PendingMotion::None;
            set_status(state, mode_name(state.mode));
            return;
        }
        if (!is_special) {
            execute_pending_motion(state, static_cast<char32_t>(key));
        }
        return;
    }

    handle_keymap_input(state, key, is_special);
}

void run_editor(EditorState &state) {
    while (!state.should_quit) {
        state.runtime.dispatch_editor_events(state.core);
        state.runtime.poll_services();
        state.runtime.take_service_events();
        int screen_rows = 0;
        int screen_cols = 0;
        getmaxyx(stdscr, screen_rows, screen_cols);
        int buffer_rows = screen_rows - 2;
        int buffer_cols = screen_cols - line_number_width(state) - 1;
        ensure_cursor_visible(state, buffer_rows, buffer_cols);
        draw_editor(state);
        handle_input(state);
    }
}

int main(int argc, char **argv) {
    initialize_locale();
    EditorState state;
    EditorConfig config;
    try {
        config = load_editor_config();
        state.keybindings = load_keybindings(config);
    } catch (const std::exception &error) {
        state.keybindings = load_embedded_keybindings();
        set_status(state, std::string("Keybindings config error: ") + error.what());
    }
    try {
        state.theme = load_theme(config);
    } catch (const std::exception &error) {
        state.theme = load_embedded_theme();
        set_status(state, std::string("Theme config error: ") + error.what());
    }
    if (argc > 1) {
        if (state.core.load_file(argv[1])) {
            set_status(state, "Opened " + std::string(argv[1]));
        } else {
            set_status(state, "Could not open file");
        }
    } else if (!config.source_path.empty()) {
        set_status(state, "Config: " + config.source_path);
    } else if (!state.keybindings.source_path.empty()) {
        set_status(state, "Keybindings: " + state.keybindings.source_path);
    }

    setup_terminal(state.theme);
    state.runtime.start_services();
    run_editor(state);
    state.runtime.stop_services();
    teardown_terminal();
    return 0;
}
