#include "editor_internal.hpp"

#include "logger.hpp"
#include "string_utils.hpp"

#ifdef _WIN32
#include "pdcurses_compat.hpp"
#endif

#include <algorithm>
#include <array>
#include <csignal>
#include <chrono>
#include <exception>
#include <cstdio>
#include <limits>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

enum class ThemeSlot : short {
    Default = 1,
    LineNumber,
    CursorLine,
    CursorLineNumber,
    StatusBar,
    MessageBar,
    CommandLine,
    Selection,
    SearchMatch,
    SearchMatchCurrent,
    SyntaxKeyword,
    SyntaxType,
    SyntaxFunction,
    SyntaxBuiltin,
    SyntaxProperty,
    SyntaxConstant,
    SyntaxNumber,
    SyntaxOperator,
    SyntaxString,
    SyntaxComment,
    DiagnosticError,
    DiagnosticWarning,
    DiagnosticMessageError,
    DiagnosticMessageWarning,
    DiagnosticSelected,
};

struct RgbColor {
    int red = 0;
    int green = 0;
    int blue = 0;
};

RgbColor xterm_palette_rgb(short color) {
    static const std::array<RgbColor, 16> ansi = {{
        {0, 0, 0},
        {205, 0, 0},
        {0, 205, 0},
        {205, 205, 0},
        {0, 0, 238},
        {205, 0, 205},
        {0, 205, 205},
        {229, 229, 229},
        {127, 127, 127},
        {255, 0, 0},
        {0, 255, 0},
        {255, 255, 0},
        {92, 92, 255},
        {255, 0, 255},
        {0, 255, 255},
        {255, 255, 255},
    }};

    if (color < 0) {
        return {};
    }
    if (color < 16) {
        return ansi[static_cast<std::size_t>(color)];
    }
    if (color >= 16 && color <= 231) {
        int index = color - 16;
        int red = index / 36;
        int green = (index / 6) % 6;
        int blue = index % 6;
        auto component = [](int value) { return value == 0 ? 0 : 55 + value * 40; };
        return {component(red), component(green), component(blue)};
    }
    if (color >= 232 && color <= 255) {
        int gray = 8 + (color - 232) * 10;
        return {gray, gray, gray};
    }
    return ansi[7];
}

short nearest_supported_color(short color, int terminal_colors) {
    if (color < 0 || terminal_colors <= 0) {
        return color;
    }
    if (color < terminal_colors) {
        return color;
    }
    int supported = std::min(terminal_colors, 16);
    RgbColor target = xterm_palette_rgb(color);
    int best_distance = std::numeric_limits<int>::max();
    short best = terminal_colors > 8 ? 7 : COLOR_WHITE;
    for (int candidate = 0; candidate < supported; ++candidate) {
        RgbColor sample = xterm_palette_rgb(static_cast<short>(candidate));
        int dr = target.red - sample.red;
        int dg = target.green - sample.green;
        int db = target.blue - sample.blue;
        int distance = dr * dr + dg * dg + db * db;
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<short>(candidate);
        }
    }
    if (terminal_colors <= 8 && best >= 8) {
        return static_cast<short>(best - 8);
    }
    return best;
}

bool g_terminal_active = false;
#if defined(__unix__) || defined(__APPLE__)
bool g_shell_termios_valid = false;
termios g_shell_termios{};
#endif

std::size_t display_width_until(const std::u32string &line, std::size_t limit, std::size_t tabstop) {
    std::size_t width = 0;
    std::size_t capped = limit > line.size() ? line.size() : limit;
    for (std::size_t i = 0; i < capped; ++i) {
        width += codepoint_display_width(line[i], width, tabstop);
    }
    return width;
}

std::size_t display_width(const std::u32string &line, std::size_t tabstop) {
    return display_width_until(line, line.size(), tabstop);
}

void ensure_horizontal_visibility(EditorState &state, std::size_t window_id, int screen_cols) {
    EditorState::WindowUiState &buffer_ui = state.window_ui(window_id);
    const EditorCore &core = state.window_core(window_id);
    Position cursor = state.displayed_cursor(window_id);
    const std::u32string &line = core.lines()[cursor.row];
    std::size_t cursor_x = display_width_until(line, cursor.column, effective_tabstop(state.config, core.file_path()));
    std::size_t usable_cols = screen_cols > 0 ? static_cast<std::size_t>(screen_cols) : 1;

    while (buffer_ui.col_offset > cursor_x) {
        --buffer_ui.col_offset;
    }
    while (cursor_x >= buffer_ui.col_offset + usable_cols) {
        ++buffer_ui.col_offset;
    }
}

void ensure_vertical_visibility(EditorState &state, std::size_t window_id, int screen_rows, int buffer_cols) {
    EditorState::WindowUiState &buffer_ui = state.window_ui(window_id);
    std::size_t usable_rows = screen_rows > 0 ? static_cast<std::size_t>(screen_rows) : 1;
    const std::vector<VisualRow> &visual_rows = visual_rows_for_window(state, window_id, buffer_cols);
    std::size_t cursor_visual_row = visual_row_for_buffer_row(visual_rows, state.displayed_cursor(window_id).row);
    if (cursor_visual_row < buffer_ui.row_offset) {
        buffer_ui.row_offset = cursor_visual_row;
    }
    while (cursor_visual_row >= buffer_ui.row_offset + usable_rows) {
        ++buffer_ui.row_offset;
    }
    if (buffer_ui.row_offset > 0 && buffer_ui.row_offset >= visual_rows.size()) {
        buffer_ui.row_offset = visual_rows.empty() ? 0 : visual_rows.size() - 1;
    }
}

void ensure_cursor_visible(EditorState &state, std::size_t window_id, int buffer_rows, int buffer_cols) {
    ensure_vertical_visibility(state, window_id, buffer_rows, buffer_cols);
    ensure_horizontal_visibility(state, window_id, buffer_cols);
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
        case StyleRole::SyntaxType:
            return ThemeSlot::SyntaxType;
        case StyleRole::SyntaxFunction:
            return ThemeSlot::SyntaxFunction;
        case StyleRole::SyntaxBuiltin:
            return ThemeSlot::SyntaxBuiltin;
        case StyleRole::SyntaxProperty:
            return ThemeSlot::SyntaxProperty;
        case StyleRole::SyntaxConstant:
            return ThemeSlot::SyntaxConstant;
        case StyleRole::SyntaxNumber:
            return ThemeSlot::SyntaxNumber;
        case StyleRole::SyntaxOperator:
            return ThemeSlot::SyntaxOperator;
        case StyleRole::SyntaxString:
            return ThemeSlot::SyntaxString;
        case StyleRole::SyntaxComment:
            return ThemeSlot::SyntaxComment;
        case StyleRole::DiagnosticError:
            return ThemeSlot::DiagnosticError;
        case StyleRole::DiagnosticWarning:
            return ThemeSlot::DiagnosticWarning;
        case StyleRole::DiagnosticMessageError:
            return ThemeSlot::DiagnosticMessageError;
        case StyleRole::DiagnosticMessageWarning:
            return ThemeSlot::DiagnosticMessageWarning;
        case StyleRole::DiagnosticSelected:
            return ThemeSlot::DiagnosticSelected;
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

StyleRole resolve_style_role(Position position, const std::vector<HighlightSpan> &spans, StyleRole base_role) {
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

std::vector<HighlightSpan> collect_line_highlights(const EditorState &state, std::size_t window_id, std::size_t row) {
    const EditorCore &core = state.window_core(window_id);
    const EditorState::WindowUiState &window_state = state.window_ui(window_id);
    const EditorState::BufferUiState &buffer_ui = state.buffer_ui_state(state.window_buffer(window_id).id);
    std::vector<HighlightSpan> spans;
    Range entire_line = core.line_range(row);
    const auto &syntax_ui = state.buffer_syntax_ui(state.window_buffer(window_id).id);
    if (row < syntax_ui.syntax_highlights.size()) {
        spans.insert(spans.end(), syntax_ui.syntax_highlights[row].begin(), syntax_ui.syntax_highlights[row].end());
    }
    if (state.displayed_cursor(window_id).row == row) {
        spans.push_back({entire_line, StyleRole::CursorLine, 10});
    }
    std::optional<Range> selection = core.selection_range();
    if (selection && row >= selection->start.row && row <= selection->end.row) {
        Range line_selection{
            {row, row == selection->start.row ? selection->start.column : 0},
            {row, row == selection->end.row ? selection->end.column : core.line_length(row)}};
        if (!(line_selection.start == line_selection.end)) {
            spans.push_back({line_selection, StyleRole::Selection, 100});
        }
    }
    for (std::size_t index = 0; index < buffer_ui.search_matches.size(); ++index) {
        const Range &match = buffer_ui.search_matches[index];
        if (row < match.start.row || row > match.end.row) {
            continue;
        }
        Range line_match{
            {row, row == match.start.row ? match.start.column : 0},
            {row, row == match.end.row ? match.end.column : core.line_length(row)}};
        if ((line_match.start == line_match.end)) {
            continue;
        }
        StyleRole role =
            window_state.current_search_match_index && *window_state.current_search_match_index == index
                ? StyleRole::SearchMatchCurrent
                : StyleRole::SearchMatch;
        int priority = role == StyleRole::SearchMatchCurrent ? 95 : 90;
        spans.push_back({line_match, role, priority});
    }
    if (state.should_render_diagnostics(window_id)) {
        for (const Diagnostic &diagnostic : core.diagnostics()) {
            Range range = normalized_range(diagnostic.range);
            if (row < range.start.row || row > range.end.row) {
                continue;
            }
            Range line_diagnostic{
                {row, row == range.start.row ? range.start.column : 0},
                {row, row == range.end.row ? range.end.column : core.line_length(row)}};
            if ((line_diagnostic.start == line_diagnostic.end)) {
                continue;
            }
            StyleRole role = diagnostic.severity == DiagnosticSeverity::Error
                                 ? StyleRole::DiagnosticError
                                 : StyleRole::DiagnosticWarning;
            spans.push_back({line_diagnostic, role, 80});
        }
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

void render_styled_tab(const Theme &theme, int screen_row, int screen_col, int width, StyleRole role) {
    std::string filler(static_cast<std::size_t>(width), ' ');
    TextStyle style = theme_style(theme, role);
    attr_t attrs = static_cast<attr_t>(curses_attributes(style, role));
    mvaddnstr(screen_row, screen_col, filler.c_str(), width);
    mvchgat(screen_row, screen_col, width, attrs, static_cast<short>(theme_slot(role)), nullptr);
}

void draw_styled_line(
    const EditorState &state,
    int screen_row,
    int start_col,
    const std::u32string &line,
    const std::vector<HighlightSpan> &spans,
    std::size_t row,
    std::size_t tabstop,
    std::size_t col_offset,
    int max_cols) {
    std::size_t skipped_width = 0;
    std::size_t visual_width = 0;
    int screen_col = start_col;

    for (std::size_t column = 0; column < line.size(); ++column) {
        char32_t codepoint = line[column];
        int width = static_cast<int>(codepoint_display_width(codepoint, visual_width, tabstop));
        if (skipped_width + static_cast<std::size_t>(width) <= col_offset) {
            skipped_width += static_cast<std::size_t>(width);
            visual_width += static_cast<std::size_t>(width);
            continue;
        }
        if (screen_col + width > start_col + max_cols) {
            break;
        }
        StyleRole role = resolve_style_role({row, column}, spans, StyleRole::DefaultText);
        if (codepoint == U'\t') {
            render_styled_tab(state.theme, screen_row, screen_col, width, role);
        } else {
            render_styled_glyph(state.theme, screen_row, screen_col, codepoint, role);
        }
        screen_col += width;
        visual_width += static_cast<std::size_t>(width);
    }
}

void clear_rect_line(int screen_row, int left, int width) {
    if (width <= 0) {
        return;
    }
    std::string spaces(static_cast<std::size_t>(width), ' ');
    mvaddnstr(screen_row, left, spaces.c_str(), width);
}

void draw_line_number(
    const Theme &theme,
    int screen_row,
    int start_col,
    std::size_t line_number,
    int line_number_width_value,
    StyleRole role) {
    TextStyle style = theme_style(theme, role);
    attron(curses_attributes(style, role));
    mvprintw(screen_row, start_col, "%*zu ", line_number_width_value, line_number);
    attroff(curses_attributes(style, role));
}

StyleRole annotation_role(const InlineAnnotation &annotation) {
    switch (annotation.severity) {
        case AnnotationSeverity::Error:
            return StyleRole::DiagnosticMessageError;
        case AnnotationSeverity::Warning:
            return StyleRole::DiagnosticMessageWarning;
        case AnnotationSeverity::Info:
            return StyleRole::MessageBar;
    }
    return StyleRole::MessageBar;
}

std::u32string annotation_prefix(const InlineAnnotation &annotation) {
    std::ostringstream prefix;
    prefix << (annotation.severity == AnnotationSeverity::Error ? "E" :
               annotation.severity == AnnotationSeverity::Warning ? "W" : "I");
    Range range = normalized_range(annotation.range);
    prefix << " " << (range.start.row + 1) << ":" << (range.start.column + 1);
    if (!annotation.source.empty()) {
        prefix << " " << annotation.source;
    }
    prefix << " ";
    return utf8_to_u32(prefix.str());
}

}  // namespace

void apply_theme_to_terminal(const Theme &theme) {
    start_color();
    use_default_colors();
    int terminal_colors = has_colors() ? COLORS : 0;
    for (int role_index = 0; role_index <= static_cast<int>(StyleRole::DiagnosticSelected); ++role_index) {
        StyleRole role = static_cast<StyleRole>(role_index);
        TextStyle style = theme_style(theme, role);
        init_pair(
            static_cast<short>(theme_slot(role)),
            nearest_supported_color(style.foreground, terminal_colors),
            nearest_supported_color(style.background, terminal_colors));
    }
}

void restore_shell_terminal_state() {
#if defined(__unix__) || defined(__APPLE__)
    if (g_shell_termios_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_shell_termios);
    }
#endif
}

void setup_terminal(const Theme &theme) {
#if defined(__unix__) || defined(__APPLE__)
    g_shell_termios_valid = tcgetattr(STDIN_FILENO, &g_shell_termios) == 0;
#endif
    initscr();
#if defined(SIGTSTP)
    // Ensure SIGTSTP has default handler (raw() mode might have disabled it)
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTSTP, &sa, nullptr);
#endif
    set_escdelay(25);
    def_shell_mode();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    timeout(-1);
    mouseinterval(150);
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED, nullptr);
    apply_theme_to_terminal(theme);
    curs_set(1);
    def_prog_mode();
    g_terminal_active = true;
}

void teardown_terminal() {
    if (!g_terminal_active) {
        restore_shell_terminal_state();
        return;
    }
    mousemask(0, nullptr);
    keypad(stdscr, FALSE);
    timeout(-1);
    curs_set(1);
    nl();
    noraw();
    nocbreak();
    echo();
    clear();
    refresh();
    endwin();
    reset_shell_mode();
    restore_shell_terminal_state();
    g_terminal_active = false;
}

void suspend_editor(EditorState &state) {
#if defined(SIGTSTP)
    log_debug("suspend_editor called - attempting to suspend");
    state.pending.tokens.clear();
    state.pending.motion = PendingMotion::None;
    state.pending.motion_repeat_count = 1;
    state.pending.repeat_digits.clear();
    def_prog_mode();
    endwin();
    restore_shell_terminal_state();

    // Re-enable default SIGTSTP handler before raising
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTSTP, &sa, nullptr);

    log_debug("suspend_editor sending SIGTSTP to process group");
    // Send SIGTSTP to the process group (not just this thread)
    int result = kill(0, SIGTSTP);
    if (result != 0) {
        log_debug("suspend_editor kill() failed errno=" + std::to_string(errno));
    }

    // Execution continues here after 'fg'
    log_debug("suspend_editor resumed after SIGTSTP");
    reset_prog_mode();
    refresh();
    clearok(stdscr, TRUE);
    state.set_status(mode_name(state.mode));
#else
    log_debug("suspend_editor called but SIGTSTP not defined");
    state.set_status("Suspend not supported");
#endif
}

int line_number_width(const EditorCore &core) {
    std::size_t line_count = core.line_count();
    int width = 1;
    while (line_count >= 10) {
        line_count /= 10;
        ++width;
    }
    return width;
}

void draw_buffer_rows(const EditorState &state, std::size_t window_id, const WindowLayoutRect &rect, int line_number_width_value) {
    const EditorCore &core = state.window_core(window_id);
    const EditorState::WindowUiState &buffer_ui = state.window_ui(window_id);
    int buffer_rows = rect.height;
    int buffer_cols = rect.width - line_number_width_value - 1;
    if (buffer_rows <= 0 || buffer_cols <= 0) {
        return;
    }
    std::size_t tabstop = effective_tabstop(state.config, core.file_path());
    const std::vector<VisualRow> &visual_rows = visual_rows_for_window(state, window_id, buffer_cols);
    for (int row_offset = 0; row_offset < buffer_rows; ++row_offset) {
        int screen_row = rect.top + row_offset;
        std::size_t visual_row_index = buffer_ui.row_offset + static_cast<std::size_t>(row_offset);
        clear_rect_line(screen_row, rect.left, rect.width);
        if (visual_row_index >= visual_rows.size()) {
            if (rect.width > 0) {
                mvaddch(screen_row, rect.left, '~');
            }
            continue;
        }

        const VisualRow &visual_row = visual_rows[visual_row_index];
        if (visual_row.kind == VisualRowKind::SourceLine) {
            StyleRole line_number_role =
                state.displayed_cursor(window_id).row == visual_row.buffer_row ? StyleRole::CursorLineNumber : StyleRole::LineNumber;
            draw_line_number(state.theme, screen_row, rect.left, visual_row.buffer_row + 1, line_number_width_value, line_number_role);
            const std::u32string &line = core.lines()[visual_row.buffer_row];
            std::vector<HighlightSpan> spans = collect_line_highlights(state, window_id, visual_row.buffer_row);
            draw_styled_line(
                state,
                screen_row,
                rect.left + line_number_width_value + 1,
                line,
                spans,
                visual_row.buffer_row,
                tabstop,
                buffer_ui.col_offset,
                buffer_cols);
            continue;
        }

        const AnnotationEntryView &annotation = *visual_row.annotation;
        StyleRole role = annotation_role(annotation.annotation);
        if (annotation.diagnostic_index && buffer_ui.selected_diagnostic_index &&
            *annotation.diagnostic_index == *buffer_ui.selected_diagnostic_index) {
            role = StyleRole::DiagnosticSelected;
        }
        TextStyle style = theme_style(state.theme, role);
        attron(curses_attributes(style, role));
        mvprintw(screen_row, rect.left, "%*s ", line_number_width_value, "");
        std::vector<std::u32string> wrapped =
            wrap_annotation_text(annotation_prefix(annotation.annotation) + annotation.annotation.text, buffer_cols - 2);
        std::u32string text = visual_row.wrap_offset < wrapped.size() ? wrapped[visual_row.wrap_offset] : U"";
        int annotation_col = rect.left + line_number_width_value + 1;
        if (state.config.right_justify_diagnostics && annotation.annotation.kind == AnnotationKind::Diagnostic) {
            int rendered_width = static_cast<int>(display_width(text, tabstop));
            annotation_col += std::max(0, buffer_cols - rendered_width);
        }
        mvaddnwstr(screen_row, annotation_col, u32_to_wstring(text).c_str(), buffer_cols);
        attroff(curses_attributes(style, role));
    }
}

std::string build_status_text(const EditorState &state) {
    const EditorCore &core = state.active_core();
    Position cursor = core.cursor();
    std::string language = infer_language_id(state.config, core.file_path());
    std::string workspace = "-";
    if (const LspServerConfig *server = matching_lsp_server(state.config, core.file_path())) {
        std::filesystem::path workspace_root = infer_workspace_root(*server, core.file_path());
        if (!workspace_root.empty()) {
            workspace = workspace_root.string();
        }
    }

    std::string left_text = make_status_bar_left_text(state, core, language, workspace);
    std::string right_text = make_status_bar_right_text(core, cursor);
    return left_text + right_text;
}

void draw_status_bar(const EditorState &state, int screen_rows, int screen_cols) {
    TextStyle style = theme_style(state.theme, StyleRole::StatusBar);
    attron(curses_attributes(style, StyleRole::StatusBar));
    move(screen_rows - 2, 0);
    clrtoeol();
    const EditorCore &core = state.active_core();
    Position cursor = core.cursor();
    std::string language = infer_language_id(state.config, core.file_path());
    std::string workspace = "-";
    if (const LspServerConfig *server = matching_lsp_server(state.config, core.file_path())) {
        std::filesystem::path workspace_root = infer_workspace_root(*server, core.file_path());
        if (!workspace_root.empty()) {
            workspace = workspace_root.string();
        }
    }
    std::string left_text = make_status_bar_left_text(state, core, language, workspace);
    std::string right_text = make_status_bar_right_text(core, cursor);
    std::size_t total_width = screen_cols > 0 ? static_cast<std::size_t>(screen_cols) : 0;
    std::string status;
    if (right_text.size() >= total_width) {
        status = ellipsize_middle(right_text, total_width);
    } else {
        std::size_t left_width = total_width - right_text.size();
        status = ellipsize_middle(left_text, left_width) + right_text;
    }
    mvaddnstr(screen_rows - 2, 0, status.c_str(), screen_cols);
    attroff(curses_attributes(style, StyleRole::StatusBar));
}

void draw_message_bar(const EditorState &state, int screen_rows, int screen_cols) {
    move(screen_rows - 1, 0);
    clrtoeol();
    if (state.mode == Mode::Command) {
        TextStyle style = theme_style(state.theme, StyleRole::CommandLine);
        attron(curses_attributes(style, StyleRole::CommandLine));
        std::string prompt = ":";
        if (state.command_prompt_kind == CommandPromptKind::FilterSelection) {
            prompt = "!";
        } else if (state.command_prompt_kind == CommandPromptKind::SedSelection) {
            prompt = "S";
        }
        std::string command = prompt + u32_to_utf8(state.command_buffer);
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

void draw_popup(const EditorState &state, int screen_rows, int screen_cols) {
    if (!state.popup.visible || screen_rows <= 4 || screen_cols <= 8) {
        return;
    }

    int available_rows = screen_rows - 2;
    int max_text_width = std::max(12, screen_cols - 8);
    std::size_t menu_visible_rows = popup_menu_visible_rows_for_screen(screen_rows);
    std::size_t menu_scroll_offset = 0;
    std::string popup_title = state.popup.title;
    std::string filter_text = state.popup.kind == PopupKind::Menu ? u32_to_utf8(state.popup.filter) : "";
    std::vector<std::u32string> wrapped;
    if (state.popup.kind == PopupKind::Menu || state.popup.kind == PopupKind::KeyHints) {
        std::size_t filtered_size = state.popup.filtered_indices.size();
        std::size_t max_offset = filtered_size > menu_visible_rows ? filtered_size - menu_visible_rows : 0;
        menu_scroll_offset = std::min(state.popup.scroll_offset, max_offset);
        std::size_t item_count = filtered_size > menu_scroll_offset
            ? std::min<std::size_t>(filtered_size - menu_scroll_offset, menu_visible_rows)
            : 0;
        for (std::size_t i = 0; i < item_count; ++i) {
            const PopupMenuItem &item = state.popup.items[state.popup.filtered_indices[menu_scroll_offset + i]];
            std::string line = item.label;
            if (!item.detail.empty()) {
                line += "  " + item.detail;
            }
            wrapped.push_back(utf8_to_u32(line));
        }
        if (wrapped.empty()) {
            wrapped.push_back(U"(no matches)");
        }
    } else {
        wrapped = wrap_annotation_text(state.popup.text, max_text_width - 4);
        if (wrapped.empty()) {
            wrapped.push_back(U"");
        }
    }

    std::size_t title_width = display_width(utf8_to_u32(popup_title), 8);
    std::size_t content_width = title_width;
    for (const std::u32string &line : wrapped) {
        content_width = std::max(content_width, display_width(line, 8));
    }
    if (state.popup.kind == PopupKind::Menu) {
        std::u32string prompt_line = utf8_to_u32(filter_text.empty() ? " filter..." : filter_text);
        content_width = std::max<std::size_t>(content_width, display_width(prompt_line, 8) + 4);
        content_width = std::max<std::size_t>(content_width, 28);
    } else if (state.popup.kind == PopupKind::KeyHints) {
        content_width = std::max<std::size_t>(content_width, 28);
    }

    int popup_width = std::min(
        screen_cols - 4,
        static_cast<int>(content_width) + ((state.popup.kind == PopupKind::Menu || state.popup.kind == PopupKind::KeyHints) ? 6 : 4));
    int popup_height = 0;
    if (state.popup.kind == PopupKind::Menu) {
        popup_height = std::min(available_rows, static_cast<int>(wrapped.size()) + 5);
    } else if (state.popup.kind == PopupKind::KeyHints) {
        popup_height = std::min(available_rows, static_cast<int>(wrapped.size()) + 4);
    } else {
        popup_height = std::min(available_rows, static_cast<int>(wrapped.size()) + 2);
    }
    int top = std::max(0, (available_rows - popup_height) / 2);
    int left = std::max(0, (screen_cols - popup_width) / 2);

    int border_attrs = curses_attributes(theme_style(state.theme, StyleRole::StatusBar), StyleRole::StatusBar);
    int header_attrs = border_attrs;
    int filter_attrs = curses_attributes(theme_style(state.theme, StyleRole::CommandLine), StyleRole::CommandLine);
    int body_attrs = curses_attributes(theme_style(state.theme, StyleRole::MessageBar), StyleRole::MessageBar);
    int selected_attrs = curses_attributes(theme_style(state.theme, StyleRole::Selection), StyleRole::Selection);
    int line_number_attrs = curses_attributes(theme_style(state.theme, StyleRole::LineNumber), StyleRole::LineNumber);

    if (left + popup_width + 1 < screen_cols && top + popup_height < available_rows) {
        attrset(static_cast<attr_t>(line_number_attrs));
        for (int row = 1; row < popup_height; ++row) {
            mvaddch(top + row, left + popup_width, ' ');
        }
        for (int col = 2; col < popup_width; ++col) {
            mvaddch(top + popup_height, left + col, ' ');
        }
    }

    for (int row = 0; row < popup_height; ++row) {
        for (int col = 0; col < popup_width; ++col) {
            attrset(static_cast<attr_t>(body_attrs));
            mvaddch(top + row, left + col, ' ');
        }
    }

    attrset(static_cast<attr_t>(border_attrs));
    mvaddch(top, left, ACS_ULCORNER);
    mvaddch(top, left + popup_width - 1, ACS_URCORNER);
    mvaddch(top + popup_height - 1, left, ACS_LLCORNER);
    mvaddch(top + popup_height - 1, left + popup_width - 1, ACS_LRCORNER);
    mvhline(top, left + 1, ACS_HLINE, popup_width - 2);
    mvhline(top + popup_height - 1, left + 1, ACS_HLINE, popup_width - 2);
    mvvline(top + 1, left, ACS_VLINE, popup_height - 2);
    mvvline(top + 1, left + popup_width - 1, ACS_VLINE, popup_height - 2);

    if (state.popup.kind == PopupKind::Menu) {
        attrset(static_cast<attr_t>(header_attrs));
        mvhline(top + 1, left + 1, ' ', popup_width - 2);
        if (!popup_title.empty()) {
            std::string title = "  " + popup_title + "  ";
            mvaddnstr(top + 1, left + 2, title.c_str(), std::max(0, popup_width - 4));
        }

        attrset(static_cast<attr_t>(filter_attrs));
        mvhline(top + 2, left + 1, ' ', popup_width - 2);
        mvaddch(top + 2, left + 2, ACS_RARROW);
        std::string prompt = filter_text.empty() ? " filter..." : " " + filter_text;
        mvaddnstr(top + 2, left + 3, prompt.c_str(), std::max(0, popup_width - 5));
        attrset(static_cast<attr_t>(line_number_attrs));
        mvhline(top + 3, left + 1, ACS_HLINE, popup_width - 2);
    } else if (state.popup.kind == PopupKind::KeyHints) {
        attrset(static_cast<attr_t>(header_attrs));
        mvhline(top + 1, left + 1, ' ', popup_width - 2);
        if (!popup_title.empty()) {
            std::string title = "  " + popup_title + "  ";
            mvaddnstr(top + 1, left + 2, title.c_str(), std::max(0, popup_width - 4));
        }
        attrset(static_cast<attr_t>(line_number_attrs));
        mvhline(top + 2, left + 1, ACS_HLINE, popup_width - 2);
    } else if (!popup_title.empty()) {
        std::string title = " " + popup_title + " ";
        mvaddnstr(top, left + 1, title.c_str(), std::max(0, popup_width - 2));
    }

    attrset(static_cast<attr_t>(body_attrs));
    int content_top = top + 1;
    int content_left = left + 2;
    int content_width_chars = popup_width - 4;
    int content_rows = popup_height - 2;
    if (state.popup.kind == PopupKind::Menu) {
        content_top = top + 4;
        content_left = left + 3;
        content_width_chars = popup_width - 5;
        content_rows = popup_height - 5;
    } else if (state.popup.kind == PopupKind::KeyHints) {
        content_top = top + 3;
        content_left = left + 3;
        content_width_chars = popup_width - 5;
        content_rows = popup_height - 4;
    }
    for (int row = 0; row < content_rows && row < static_cast<int>(wrapped.size()); ++row) {
        attrset(static_cast<attr_t>(body_attrs));
        clear_rect_line(content_top + row, left + 1, popup_width - 2);
        if (state.popup.kind == PopupKind::Menu &&
            static_cast<std::size_t>(row) + menu_scroll_offset == state.popup.selected_index) {
            attrset(static_cast<attr_t>(selected_attrs));
            mvhline(content_top + row, left + 1, ' ', popup_width - 2);
            mvaddch(content_top + row, left + 2, ACS_RARROW);
        } else if (state.popup.kind == PopupKind::Menu || state.popup.kind == PopupKind::KeyHints) {
            attrset(static_cast<attr_t>(body_attrs));
            attrset(static_cast<attr_t>(line_number_attrs));
            mvaddch(content_top + row, left + 2, ACS_BULLET);
            attrset(static_cast<attr_t>(body_attrs));
        }
        mvaddnwstr(
            content_top + row,
            content_left,
            u32_to_wstring(wrapped[static_cast<std::size_t>(row)]).c_str(),
            content_width_chars);
    }

    if ((state.popup.kind == PopupKind::Menu || state.popup.kind == PopupKind::KeyHints) &&
        state.popup.filtered_indices.size() > menu_visible_rows && content_rows > 0) {
        std::size_t total = state.popup.filtered_indices.size();
        std::size_t thumb_size = std::max<std::size_t>(1, (content_rows * content_rows) / total);
        std::size_t max_scroll = total > menu_visible_rows ? total - menu_visible_rows : 0;
        std::size_t thumb_top = max_scroll == 0 ? 0 : (menu_scroll_offset * (content_rows - thumb_size)) / max_scroll;
        for (int row = 0; row < content_rows; ++row) {
            attrset(static_cast<attr_t>(line_number_attrs));
            chtype ch = (static_cast<std::size_t>(row) >= thumb_top && static_cast<std::size_t>(row) < thumb_top + thumb_size)
                ? ACS_CKBOARD
                : ACS_VLINE;
            mvaddch(content_top + row, left + popup_width - 2, ch);
        }
    }
    attrset(A_NORMAL);
}

std::pair<int, int> cursor_screen_position(const EditorState &state, const WindowLayoutRect &rect) {
    const std::size_t window_id = state.windows.active_window_id();
    const EditorCore &core = state.active_core();
    const EditorState::WindowUiState &buffer_ui = state.active_buffer_ui();
    int line_number_cols = line_number_width(core);
    int buffer_cols = rect.width - line_number_cols - 1;
    const std::vector<VisualRow> &visual_rows = visual_rows_for_window(state, window_id, buffer_cols);
    Position cursor = state.displayed_cursor(window_id);
    std::size_t visual_row_index = visual_row_for_buffer_row(visual_rows, cursor.row);
    const std::u32string &line = core.lines()[cursor.row];
    std::size_t width = display_width_until(line, cursor.column, effective_tabstop(state.config, core.file_path()));
    int screen_row = rect.top + static_cast<int>(visual_row_index - buffer_ui.row_offset);
    int screen_col = rect.left + static_cast<int>(width - buffer_ui.col_offset) + line_number_cols + 1;
    return {screen_row, screen_col};
}

std::size_t column_for_display_width(const std::u32string &line, std::size_t target_width, std::size_t tabstop) {
    std::size_t width = 0;
    for (std::size_t column = 0; column < line.size(); ++column) {
        std::size_t next_width = width + codepoint_display_width(line[column], width, tabstop);
        if (target_width < next_width) {
            return column;
        }
        width = next_width;
    }
    return line.size();
}

std::optional<ClickedBufferPosition> buffer_position_from_screen_point(const EditorState &state, int screen_row, int screen_col) {
    int total_rows = 0;
    int total_cols = 0;
    getmaxyx(stdscr, total_rows, total_cols);
    for (const WindowLayoutRect &rect : state.windows.layout_rects(total_rows, total_cols, 2)) {
        if (screen_row < rect.top || screen_row >= rect.top + rect.height ||
            screen_col < rect.left || screen_col >= rect.left + rect.width) {
            continue;
        }

        const EditorCore &core = state.window_core(rect.window_id);
        const EditorState::WindowUiState &buffer_ui = state.window_ui(rect.window_id);
        int line_number_cols = line_number_width(core);
        int buffer_cols = rect.width - line_number_cols - 1;
        if (buffer_cols <= 0) {
            return std::nullopt;
        }
        const std::vector<VisualRow> &visual_rows = visual_rows_for_window(state, rect.window_id, buffer_cols);
        std::size_t visual_row_index = buffer_ui.row_offset + static_cast<std::size_t>(screen_row - rect.top);
        if (visual_row_index >= visual_rows.size()) {
            return std::nullopt;
        }
        const VisualRow &visual_row = visual_rows[visual_row_index];
        std::size_t row = visual_row.buffer_row;

        int gutter_start = rect.left + line_number_cols + 1;
        std::size_t visual_column = buffer_ui.col_offset;
        if (screen_col > gutter_start) {
            visual_column += static_cast<std::size_t>(screen_col - gutter_start);
        }

        const std::u32string &line = core.lines()[row];
        return ClickedBufferPosition{
            rect.window_id,
            Position{row, column_for_display_width(line, visual_column, effective_tabstop(state.config, core.file_path()))}};
    }
    return std::nullopt;
}

void draw_editor(const EditorState &state) {
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);

    erase();
    std::vector<WindowLayoutRect> rects = state.windows.layout_rects(screen_rows, screen_cols, 2);
    for (const WindowLayoutRect &rect : rects) {
        int line_cols = line_number_width(state.window_core(rect.window_id));
        draw_buffer_rows(state, rect.window_id, rect, line_cols);
    }
    draw_status_bar(state, screen_rows, screen_cols);
    draw_message_bar(state, screen_rows, screen_cols);
    draw_popup(state, screen_rows, screen_cols);

    auto active_rect = std::find_if(
        rects.begin(),
        rects.end(),
        [&state](const WindowLayoutRect &rect) { return rect.window_id == state.windows.active_window_id(); });
    if (active_rect != rects.end()) {
        auto [cursor_row, cursor_col] = cursor_screen_position(state, *active_rect);
        move(cursor_row, cursor_col);
    }
    refresh();
}

void EditorState::refresh_syntax_highlights(std::size_t window_id) {
    constexpr auto kInsertSyntaxDebounce = std::chrono::milliseconds(120);
    EditorCore &core = window_core(window_id);
    SyntaxUiState &syntax_ui = buffer_syntax_ui(window_buffer(window_id).id);
    std::size_t current_revision = core.current_revision();
    SyntaxSelection selection;
    try {
        selection = resolve_syntax_selection(config, core.file_path());
    } catch (const std::exception &error) {
        if (!syntax_ui.syntax_config_error_reported) {
            set_status(std::string("Syntax config error: ") + error.what());
            syntax_ui.syntax_config_error_reported = true;
        }
        selection = {};
    }

    if (syntax_ui.pending_syntax_revision != current_revision) {
        syntax_ui.pending_syntax_revision = current_revision;
        syntax_ui.syntax_dirty_since = std::chrono::steady_clock::now();
    }

    bool syntax_stale =
        syntax_ui.syntax_revision != current_revision || syntax_ui.syntax_selection != selection ||
        syntax_ui.syntax_file_path != core.file_path();
    if (!syntax_stale) {
        return;
    }

    if (mode == Mode::Insert && syntax_ui.syntax_revision != current_revision) {
        auto now = std::chrono::steady_clock::now();
        if (now - syntax_ui.syntax_dirty_since < kInsertSyntaxDebounce) {
            return;
        }
    }

    if (syntax_ui.syntax_revision == current_revision && syntax_ui.syntax_selection == selection &&
        syntax_ui.syntax_file_path == core.file_path()) {
        return;
    }

    syntax_ui.syntax_selection = selection;
    syntax_ui.syntax_file_path = core.file_path();
    syntax_ui.syntax_revision = current_revision;
    auto syntax_result = highlight_document_syntax(core.lines(), config, syntax_ui.syntax_selection);
    if (!syntax_result) {
        syntax_ui.syntax_highlights.assign(core.lines().size(), {});
        if (!syntax_ui.syntax_config_error_reported) {
            set_status("Syntax error: " + syntax_result.error());
            syntax_ui.syntax_config_error_reported = true;
        }
    } else {
        syntax_ui.syntax_highlights = std::move(*syntax_result);
        syntax_ui.syntax_config_error_reported = false;
    }
}

void render_frame(EditorState &state) {
    state.normalize_selected_diagnostic();
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);
    for (const EditorWindow &window : state.windows.windows()) {
        state.refresh_search_matches_for_window(window.id);
        state.refresh_syntax_highlights(window.id);
    }
    auto rects = state.windows.layout_rects(screen_rows, screen_cols, 2);
    auto active_rect = std::find_if(
        rects.begin(),
        rects.end(),
        [&state](const WindowLayoutRect &rect) { return rect.window_id == state.windows.active_window_id(); });
    if (active_rect != rects.end()) {
        int buffer_rows = active_rect->height;
        int buffer_cols = active_rect->width - line_number_width(state.active_core()) - 1;
        ensure_cursor_visible(state, state.windows.active_window_id(), buffer_rows, buffer_cols);
    }
    draw_editor(state);
}
