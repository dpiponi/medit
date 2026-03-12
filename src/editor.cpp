#include "config.hpp"
#include "editor_commands.hpp"
#include "editor_core.hpp"
#include "editor_session.hpp"
#include "keybindings.hpp"
#include "logger.hpp"
#include "lsp_service.hpp"
#include "process_utils.hpp"
#include "services.hpp"
#include "syntax.hpp"
#include "string_utils.hpp"
#include "theme.hpp"

#include <algorithm>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <map>
#include <curses.h>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

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

enum class CommandPromptKind {
    EditorCommand,
    FilterSelection,
    SedSelection,
};

struct EditorState {
    struct PromptHistory {
        std::vector<std::u32string> entries;
        std::optional<std::size_t> browse_index;
        std::u32string draft;
    };

    struct JumpLocation {
        std::string document_uri;
        std::optional<std::string> file_path;
        Position position;
    };

    struct RecordedInput {
        std::string token;
        wint_t key = 0;
        bool printable = false;
    };

    struct BufferUiState {
        std::size_t row_offset = 0;
        std::size_t col_offset = 0;
        std::u32string active_search_pattern;
        std::vector<Range> search_matches;
        std::optional<std::size_t> current_search_match_index;
        Position search_origin;
        bool search_pattern_valid = true;
        std::string compiled_search_pattern_utf8;
        std::unique_ptr<std::regex> compiled_search_regex;
        std::size_t search_matches_version = 0;
        std::optional<std::size_t> selected_diagnostic_index;
        SyntaxSelection syntax_selection;
        std::vector<std::vector<HighlightSpan>> syntax_highlights;
        std::size_t syntax_revision = std::numeric_limits<std::size_t>::max();
        std::optional<std::string> syntax_file_path;
        bool syntax_config_error_reported = false;
    };

    EditorSession session;
    EditorRuntime runtime;
    EditorConfig config;
    KeyBindings keybindings;
    Theme theme = load_embedded_theme();
    std::map<std::size_t, BufferUiState> buffer_ui;
    bool should_quit = false;
    Mode mode = Mode::Normal;
    CommandPromptKind command_prompt_kind = CommandPromptKind::EditorCommand;
    std::u32string command_buffer;
    PromptHistory editor_command_history;
    PromptHistory filter_command_history;
    PromptHistory sed_command_history;
    std::u32string search_buffer;
    std::string status_message = "NORMAL";
    std::vector<std::string> pending_tokens;
    PendingMotion pending_motion = PendingMotion::None;
    std::string repeat_digits;
    std::size_t pending_motion_repeat_count = 1;
    std::size_t replay_depth = 0;
    std::size_t group_depth = 0;
    std::size_t group_repeat_count = 1;
    std::vector<RecordedInput> group_inputs;
    std::vector<JumpLocation> jump_back_stack;
    std::vector<JumpLocation> jump_forward_stack;
};

EditorBuffer &active_buffer(EditorState &state) {
    return state.session.active_buffer();
}

const EditorBuffer &active_buffer(const EditorState &state) {
    return state.session.active_buffer();
}

EditorCore &active_core(EditorState &state) {
    return active_buffer(state).core;
}

const EditorCore &active_core(const EditorState &state) {
    return active_buffer(state).core;
}

EditorState::BufferUiState &active_buffer_ui(EditorState &state) {
    return state.buffer_ui.try_emplace(state.session.active_buffer_id()).first->second;
}

const EditorState::BufferUiState &active_buffer_ui(const EditorState &state) {
    auto found = state.buffer_ui.find(state.session.active_buffer_id());
    if (found != state.buffer_ui.end()) {
        return found->second;
    }
    return const_cast<EditorState &>(state).buffer_ui.try_emplace(state.session.active_buffer_id()).first->second;
}

EditorState::PromptHistory &active_prompt_history(EditorState &state) {
    switch (state.command_prompt_kind) {
        case CommandPromptKind::EditorCommand:
            return state.editor_command_history;
        case CommandPromptKind::FilterSelection:
            return state.filter_command_history;
        case CommandPromptKind::SedSelection:
            return state.sed_command_history;
    }
    return state.editor_command_history;
}

void reset_prompt_history_navigation(EditorState::PromptHistory &history) {
    history.browse_index.reset();
    history.draft.clear();
}

void add_prompt_history_entry(EditorState &state, const std::u32string &entry) {
    if (entry.empty()) {
        return;
    }
    EditorState::PromptHistory &history = active_prompt_history(state);
    if (history.entries.empty() || history.entries.back() != entry) {
        history.entries.push_back(entry);
    }
    reset_prompt_history_navigation(history);
}

void browse_prompt_history(EditorState &state, bool previous) {
    EditorState::PromptHistory &history = active_prompt_history(state);
    if (history.entries.empty()) {
        return;
    }
    if (!history.browse_index) {
        if (!previous) {
            return;
        }
        history.draft = state.command_buffer;
        history.browse_index = history.entries.size() - 1;
    } else if (previous) {
        if (*history.browse_index == 0) {
            return;
        }
        --*history.browse_index;
    } else {
        if (*history.browse_index + 1 >= history.entries.size()) {
            state.command_buffer = history.draft;
            reset_prompt_history_navigation(history);
            return;
        }
        ++*history.browse_index;
    }
    state.command_buffer = history.entries[*history.browse_index];
}

struct DiagnosticEntryView {
    std::size_t index = 0;
    Diagnostic diagnostic;
};

struct AnnotationEntryView {
    std::optional<std::size_t> diagnostic_index;
    InlineAnnotation annotation;
};

enum class VisualRowKind {
    SourceLine,
    Annotation,
};

struct VisualRow {
    VisualRowKind kind = VisualRowKind::SourceLine;
    std::size_t buffer_row = 0;
    std::size_t wrap_offset = 0;
    std::optional<AnnotationEntryView> annotation;
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
    SyntaxType = 12,
    SyntaxFunction = 13,
    SyntaxBuiltin = 14,
    SyntaxProperty = 15,
    SyntaxConstant = 16,
    SyntaxNumber = 17,
    SyntaxOperator = 18,
    SyntaxString = 19,
    SyntaxComment = 20,
    DiagnosticError = 21,
    DiagnosticWarning = 22,
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

void invalidate_syntax_cache(EditorState &state) {
    for (auto &[buffer_id, buffer_ui] : state.buffer_ui) {
        (void)buffer_id;
        buffer_ui.syntax_revision = std::numeric_limits<std::size_t>::max();
        buffer_ui.syntax_selection = {};
        buffer_ui.syntax_file_path.reset();
        buffer_ui.syntax_highlights.clear();
        buffer_ui.syntax_config_error_reported = false;
    }
}

void initialize_locale() {
    setlocale(LC_ALL, "");
}

void apply_theme_to_terminal(const Theme &theme) {
    start_color();
    use_default_colors();
    for (int role_index = 0; role_index <= static_cast<int>(StyleRole::DiagnosticWarning); ++role_index) {
        StyleRole role = static_cast<StyleRole>(role_index);
        TextStyle style = theme_style(theme, role);
        init_pair(static_cast<short>(theme_slot(role)), style.foreground, style.background);
    }
}

bool g_terminal_active = false;
#if defined(__unix__) || defined(__APPLE__)
bool g_shell_termios_valid = false;
termios g_shell_termios{};
#endif

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

bool suspend_supported() {
#if defined(SIGTSTP)
    return true;
#else
    return false;
#endif
}

void suspend_editor(EditorState &state) {
#if defined(SIGTSTP)
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    def_prog_mode();
    endwin();
    restore_shell_terminal_state();
    std::raise(SIGTSTP);
    reset_prog_mode();
    refresh();
    clearok(stdscr, TRUE);
    set_status(state, mode_name(state.mode));
#else
    set_status(state, "Suspend not supported");
#endif
}

void enter_normal_mode(EditorState &state) {
    EditorCore &core = active_core(state);
    if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
        core.clear_selection();
    }
    state.mode = Mode::Normal;
    state.command_buffer.clear();
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    state.search_buffer.clear();
    Position cursor = core.cursor();
    std::size_t length = core.line_length(cursor.row);
    if (cursor.column > 0 && cursor.column == length) {
        core.set_cursor({cursor.row, cursor.column - 1});
    }
    set_status(state, mode_name(state.mode));
}

void enter_insert_mode(EditorState &state) {
    active_core(state).clear_selection();
    state.mode = Mode::Insert;
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    state.search_buffer.clear();
    set_status(state, mode_name(state.mode));
}

void enter_command_mode(EditorState &state) {
    active_core(state).clear_selection();
    state.mode = Mode::Command;
    state.command_prompt_kind = CommandPromptKind::EditorCommand;
    state.command_buffer.clear();
    reset_prompt_history_navigation(state.editor_command_history);
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    state.search_buffer.clear();
    set_status(state, ":");
}

void enter_filter_command_mode(EditorState &state) {
    if (!active_core(state).has_selection()) {
        set_status(state, "No selection");
        return;
    }
    state.mode = Mode::Command;
    state.command_prompt_kind = CommandPromptKind::FilterSelection;
    state.command_buffer.clear();
    reset_prompt_history_navigation(state.filter_command_history);
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    state.search_buffer.clear();
    set_status(state, "|");
}

void enter_sed_command_mode(EditorState &state) {
    if (!active_core(state).has_selection()) {
        set_status(state, "No selection");
        return;
    }
    state.mode = Mode::Command;
    state.command_prompt_kind = CommandPromptKind::SedSelection;
    state.command_buffer.clear();
    reset_prompt_history_navigation(state.sed_command_history);
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    state.search_buffer.clear();
    set_status(state, "S");
}

void enter_search_mode(EditorState &state) {
    EditorCore &core = active_core(state);
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    core.clear_selection();
    state.mode = Mode::Search;
    state.search_buffer = buffer_ui.active_search_pattern;
    buffer_ui.search_origin = core.cursor();
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    set_status(state, "/");
}

bool can_quit_without_force(const EditorState &state) {
    return !state.session.has_dirty_buffers();
}

void quit_editor(EditorState &state) {
    state.should_quit = true;
}

void handle_write_command(EditorState &state, const std::string &argument) {
    EditorCore &core = active_core(state);
    if (argument.empty()) {
        if (core.save_current_file()) {
            set_status(state, std::format("Wrote {}", core.display_file_name()));
        } else {
            set_status(state, core.file_path() ? "Write failed" : "No file name");
        }
        return;
    }
    if (core.save_current_file_as(argument)) {
        set_status(state, std::format("Wrote {}", argument));
    } else {
        set_status(state, "Write failed");
    }
}

void handle_edit_command(EditorState &state, const std::string &argument) {
    if (argument.empty()) {
        set_status(state, "No file name");
        return;
    }
    log_debug("edit command open path=" + argument);
    EditorBuffer *buffer = state.session.open_file(argument, true);
    if (buffer) {
        state.buffer_ui[buffer->id] = EditorState::BufferUiState{};
        log_debug("edit command opened path=" + argument);
        set_status(state, std::format("Opened {}", argument));
    } else {
        log_debug("edit command open failed path=" + argument);
        set_status(state, "Could not open file");
    }
}

std::vector<DiagnosticEntryView> sorted_diagnostics(const EditorState &state) {
    std::vector<DiagnosticEntryView> diagnostics;
    const std::vector<Diagnostic> &source = active_core(state).diagnostics();
    diagnostics.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        diagnostics.push_back({index, source[index]});
    }
    std::sort(diagnostics.begin(), diagnostics.end(), [](const DiagnosticEntryView &left, const DiagnosticEntryView &right) {
        Range left_range = normalized_range(left.diagnostic.range);
        Range right_range = normalized_range(right.diagnostic.range);
        if (positions_equal(left_range.start, right_range.start)) {
            return position_less_than(left_range.end, right_range.end);
        }
        return position_less_than(left_range.start, right_range.start);
    });
    return diagnostics;
}

void normalize_selected_diagnostic(EditorState &state) {
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    if (active_core(state).diagnostics().empty()) {
        buffer_ui.selected_diagnostic_index.reset();
        return;
    }
    if (!buffer_ui.selected_diagnostic_index || *buffer_ui.selected_diagnostic_index >= active_core(state).diagnostics().size()) {
        buffer_ui.selected_diagnostic_index = 0;
    }
}

void focus_diagnostic(EditorState &state, std::size_t diagnostic_index) {
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    EditorCore &core = active_core(state);
    const std::vector<Diagnostic> &diagnostics = core.diagnostics();
    if (diagnostic_index >= diagnostics.size()) {
        return;
    }
    buffer_ui.selected_diagnostic_index = diagnostic_index;
    Range range = normalized_range(diagnostics[diagnostic_index].range);
    core.set_cursor(range.start);
}

void show_diagnostics_summary(EditorState &state) {
    std::size_t errors = 0;
    std::size_t warnings = 0;
    for (const Diagnostic &diagnostic : active_core(state).diagnostics()) {
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            ++errors;
        } else {
            ++warnings;
        }
    }
    if (errors == 0 && warnings == 0) {
        set_status(state, "No diagnostics");
        return;
    }
    set_status(
        state,
        std::format(
            "{} error{}, {} warning{}",
            errors,
            errors == 1 ? "" : "s",
            warnings,
            warnings == 1 ? "" : "s"));
}

void navigate_diagnostic(EditorState &state, bool forward) {
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    std::vector<DiagnosticEntryView> diagnostics = sorted_diagnostics(state);
    if (diagnostics.empty()) {
        buffer_ui.selected_diagnostic_index.reset();
        set_status(state, "No diagnostics");
        return;
    }

    std::size_t current_sorted_index = 0;
    bool found_current = false;
    if (buffer_ui.selected_diagnostic_index) {
        for (std::size_t index = 0; index < diagnostics.size(); ++index) {
            if (diagnostics[index].index == *buffer_ui.selected_diagnostic_index) {
                current_sorted_index = index;
                found_current = true;
                break;
            }
        }
    }
    if (!found_current) {
        Position cursor = active_core(state).cursor();
        for (std::size_t index = 0; index < diagnostics.size(); ++index) {
            Range range = normalized_range(diagnostics[index].diagnostic.range);
            if (!position_less_than(range.start, cursor)) {
                current_sorted_index = index;
                found_current = true;
                break;
            }
        }
        if (!found_current) {
            current_sorted_index = 0;
        }
    }

    if (forward) {
        current_sorted_index = (current_sorted_index + 1) % diagnostics.size();
    } else {
        current_sorted_index = current_sorted_index == 0 ? diagnostics.size() - 1 : current_sorted_index - 1;
    }

    focus_diagnostic(state, diagnostics[current_sorted_index].index);
    const Diagnostic &diagnostic = diagnostics[current_sorted_index].diagnostic;
    set_status(
        state,
        std::string(diagnostic.severity == DiagnosticSeverity::Error ? "Error: " : "Warning: ") +
            u32_to_utf8(diagnostic.message));
}

std::vector<AnnotationEntryView> sorted_annotations(const EditorState &state) {
    std::vector<AnnotationEntryView> annotations;
    std::vector<InlineAnnotation> projected = active_core(state).projected_annotations();
    annotations.reserve(projected.size());

    std::vector<DiagnosticEntryView> diagnostics = sorted_diagnostics(state);
    for (const InlineAnnotation &annotation : projected) {
        std::optional<std::size_t> diagnostic_index;
        if (annotation.kind == AnnotationKind::Diagnostic) {
            for (const DiagnosticEntryView &diagnostic : diagnostics) {
                if (positions_equal(normalized_range(diagnostic.diagnostic.range).start, normalized_range(annotation.range).start) &&
                    positions_equal(normalized_range(diagnostic.diagnostic.range).end, normalized_range(annotation.range).end) &&
                    diagnostic.diagnostic.source == annotation.source &&
                    diagnostic.diagnostic.message == annotation.text) {
                    diagnostic_index = diagnostic.index;
                    break;
                }
            }
        }
        annotations.push_back({diagnostic_index, annotation});
    }

    std::sort(annotations.begin(), annotations.end(), [](const AnnotationEntryView &left, const AnnotationEntryView &right) {
        Range left_range = normalized_range(left.annotation.range);
        Range right_range = normalized_range(right.annotation.range);
        std::size_t left_anchor = left_range.end.row;
        std::size_t right_anchor = right_range.end.row;
        if (left_anchor != right_anchor) {
            return left_anchor < right_anchor;
        }
        if (positions_equal(left_range.start, right_range.start)) {
            return position_less_than(left_range.end, right_range.end);
        }
        return position_less_than(left_range.start, right_range.start);
    });
    return annotations;
}

std::vector<std::u32string> wrap_annotation_text(const std::u32string &text, int max_cols) {
    if (max_cols <= 1) {
        return {text};
    }

    std::vector<std::u32string> wrapped;
    std::u32string current_line;
    int current_width = 0;
    for (char32_t codepoint : text) {
        if (codepoint == U'\n') {
            wrapped.push_back(current_line);
            current_line.clear();
            current_width = 0;
            continue;
        }
        int width = codepoint_width(codepoint);
        if (!current_line.empty() && current_width + width > max_cols) {
            wrapped.push_back(current_line);
            current_line.clear();
            current_width = 0;
        }
        current_line.push_back(codepoint);
        current_width += width;
    }
    wrapped.push_back(current_line);
    return wrapped;
}

std::vector<VisualRow> build_visual_rows(const EditorState &state, int buffer_cols) {
    std::vector<VisualRow> rows;
    std::vector<AnnotationEntryView> annotations = sorted_annotations(state);
    std::size_t annotation_index = 0;

    for (std::size_t row = 0; row < active_core(state).line_count(); ++row) {
        rows.push_back({VisualRowKind::SourceLine, row, 0, std::nullopt});
        while (annotation_index < annotations.size()) {
            Range range = normalized_range(annotations[annotation_index].annotation.range);
            if (range.end.row != row) {
                break;
            }
            std::vector<std::u32string> wrapped = wrap_annotation_text(annotations[annotation_index].annotation.text, buffer_cols - 2);
            for (std::size_t wrap_index = 0; wrap_index < wrapped.size(); ++wrap_index) {
                rows.push_back({VisualRowKind::Annotation, row, wrap_index, annotations[annotation_index]});
            }
            ++annotation_index;
        }
    }

    return rows;
}

std::size_t visual_row_for_buffer_row(const std::vector<VisualRow> &rows, std::size_t buffer_row) {
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].kind == VisualRowKind::SourceLine && rows[index].buffer_row == buffer_row) {
            return index;
        }
    }
    return 0;
}

void handle_quit_command(EditorState &state, bool force) {
    if (!force && !can_quit_without_force(state)) {
        set_status(state, "Unsaved changes; use :q! to quit");
        return;
    }
    quit_editor(state);
}

void handle_write_quit_command(EditorState &state, const std::string &argument) {
    EditorCore &core = active_core(state);
    if (argument.empty()) {
        if (!core.save_current_file()) {
            set_status(state, core.file_path() ? "Write failed" : "No file name");
            return;
        }
        if (!can_quit_without_force(state)) {
            set_status(state, "Other buffers still have unsaved changes");
            return;
        }
        quit_editor(state);
        return;
    }
    if (!core.save_current_file_as(argument)) {
        set_status(state, "Write failed");
        return;
    }
    if (!can_quit_without_force(state)) {
        set_status(state, "Other buffers still have unsaved changes");
        return;
    }
    quit_editor(state);
}

std::string buffers_summary(const EditorState &state) {
    std::ostringstream message;
    const std::vector<EditorBuffer> &buffers = state.session.buffers();
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        if (index > 0) {
            message << " | ";
        }
        const EditorBuffer &buffer = buffers[index];
        if (index == state.session.active_buffer_index()) {
            message << "*";
        }
        message << buffer.id << ":" << buffer.core.display_file_name();
        if (buffer.core.is_dirty()) {
            message << "[+]";
        }
    }
    return message.str();
}

void handle_buffer_switch_command(EditorState &state, const std::string &argument) {
    if (argument.empty()) {
        set_status(state, "No buffer id");
        return;
    }
    std::size_t buffer_id = 0;
    try {
        buffer_id = static_cast<std::size_t>(std::stoul(argument));
    } catch (const std::exception &) {
        set_status(state, "Invalid buffer id");
        return;
    }
    if (state.session.switch_to_id(buffer_id)) {
        set_status(state, std::format("Switched to {}", active_core(state).display_file_name()));
    } else {
        set_status(state, "No such buffer");
    }
}

void handle_buffer_delete_command(EditorState &state, bool force) {
    std::size_t closing_id = state.session.active_buffer_id();
    std::string closing_name = active_core(state).display_file_name();
    std::vector<EditorEvent> closed_events;
    if (!state.session.close_active_buffer(force, &closed_events)) {
        set_status(state, "Unsaved changes; use :bd! to close");
        return;
    }
    for (const EditorEvent &event : closed_events) {
        state.runtime.dispatch_editor_event(event);
    }
    state.buffer_ui.erase(closing_id);
    active_buffer_ui(state);
    set_status(state, std::format("Closed {}", closing_name));
}

void handle_goto_line_command(EditorState &state, const std::string &argument) {
    if (argument.empty()) {
        set_status(state, "No line number");
        return;
    }

    std::size_t line_number = 0;
    try {
        line_number = static_cast<std::size_t>(std::stoul(argument));
    } catch (const std::exception &) {
        set_status(state, "Invalid line number");
        return;
    }

    if (line_number == 0) {
        set_status(state, "Line numbers start at 1");
        return;
    }

    EditorCore &core = active_core(state);
    std::size_t target_row = std::min(line_number - 1, core.line_count() - 1);
    core.set_cursor({target_row, 0});
    set_status(state, std::format("Line {}", target_row + 1));
}

std::string shell_single_quote(const std::string &text) {
    std::string quoted = "'";
    for (char ch : text) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::optional<std::string> run_picker_command(EditorState &state, const std::string &pipeline_command, std::string &error_message) {
#if defined(__unix__) || defined(__APPLE__)
    if (std::optional<std::string> missing = missing_executable_in_pipeline(pipeline_command)) {
        error_message = "missing executable: " + *missing;
        log_debug("picker preflight failed missing executable=" + *missing + " pipeline=" + pipeline_command);
        return std::nullopt;
    }
    char temp_path[] = "/tmp/medit-picker-XXXXXX";
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        error_message = "could not create temporary file";
        log_debug("picker temp file creation failed");
        return std::nullopt;
    }
    close(fd);

    const std::string current_directory = std::filesystem::current_path().string();
    std::string shell_command =
        "sh -c " + shell_single_quote("cd " + shell_single_quote(current_directory) + " && " + pipeline_command) +
        " > " + shell_single_quote(temp_path);
    log_debug("picker start cwd=" + current_directory + " pipeline=" + pipeline_command + " output=" + temp_path);
    log_debug("external command kind=picker spawn command=" + shell_command);
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    def_prog_mode();
    endwin();
    restore_shell_terminal_state();
    int result = std::system(shell_command.c_str());
    reset_prog_mode();
    refresh();
    clearok(stdscr, TRUE);
    log_debug("picker exit code=" + std::to_string(result));
    log_debug("external command kind=picker exit command=" + shell_command + " status=" + std::to_string(result));

    std::ifstream input(temp_path);
    std::string selection;
    std::getline(input, selection);
    const std::string raw_selection = selection;
    if (!selection.empty() && selection.back() == '\r') {
        selection.pop_back();
    }
    log_debug("picker raw selection=[" + raw_selection + "] normalized=[" + selection + "]");
    std::filesystem::remove(temp_path);

    if (result != 0) {
        error_message = "picker canceled";
        return std::nullopt;
    }
    if (selection.empty()) {
        error_message = "no selection";
        return std::nullopt;
    }
    return selection;
#else
    (void)state;
    (void)pipeline_command;
    log_debug("picker unsupported on this platform");
    error_message = "external pickers unsupported on this platform";
    return std::nullopt;
#endif
}

bool run_selection_filter_command(
    EditorState &state,
    const std::string &command,
    const std::u32string &input_text,
    std::u32string &output_text,
    std::string &error_message) {
#if defined(__unix__) || defined(__APPLE__)
    char input_path[] = "/tmp/medit-filter-in-XXXXXX";
    int input_fd = mkstemp(input_path);
    if (input_fd < 0) {
        error_message = "could not create filter input file";
        return false;
    }
    close(input_fd);

    char output_path[] = "/tmp/medit-filter-out-XXXXXX";
    int output_fd = mkstemp(output_path);
    if (output_fd < 0) {
        std::filesystem::remove(input_path);
        error_message = "could not create filter output file";
        return false;
    }
    close(output_fd);

    char error_path[] = "/tmp/medit-filter-err-XXXXXX";
    int stderr_fd = mkstemp(error_path);
    if (stderr_fd < 0) {
        std::filesystem::remove(input_path);
        std::filesystem::remove(output_path);
        error_message = "could not create filter error file";
        return false;
    }
    close(stderr_fd);

    {
        std::ofstream input_file(input_path, std::ios::binary);
        input_file << u32_to_utf8(input_text);
    }

    std::string shell_command =
        "sh -c " + shell_single_quote(command) +
        " < " + shell_single_quote(input_path) +
        " > " + shell_single_quote(output_path) +
        " 2> " + shell_single_quote(error_path);
    log_debug("external command kind=selection-filter spawn command=" + shell_command);
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    def_prog_mode();
    endwin();
    restore_shell_terminal_state();
    int result = std::system(shell_command.c_str());
    reset_prog_mode();
    refresh();
    clearok(stdscr, TRUE);
    log_debug("external command kind=selection-filter exit command=" + shell_command + " status=" + std::to_string(result));

    std::ifstream output_file(output_path, std::ios::binary);
    std::string output_utf8((std::istreambuf_iterator<char>(output_file)), std::istreambuf_iterator<char>());
    output_text = utf8_to_u32(output_utf8);

    std::ifstream stderr_file(error_path, std::ios::binary);
    std::string stderr_text((std::istreambuf_iterator<char>(stderr_file)), std::istreambuf_iterator<char>());

    std::filesystem::remove(input_path);
    std::filesystem::remove(output_path);
    std::filesystem::remove(error_path);

    if (result != 0) {
        error_message = trim_ascii_whitespace(stderr_text);
        if (error_message.empty()) {
            error_message = "filter command failed";
        }
        return false;
    }
    return true;
#else
    (void)state;
    (void)command;
    (void)input_text;
    (void)output_text;
    error_message = "selection filters unsupported on this platform";
    return false;
#endif
}

std::vector<std::string> run_capture_command(const std::string &command, const std::filesystem::path &working_directory) {
#if defined(__unix__) || defined(__APPLE__)
    char temp_path[] = "/tmp/medit-capture-XXXXXX";
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        log_debug("capture temp file creation failed command=" + command);
        return {};
    }
    close(fd);

    std::string shell_command =
        "sh -c " + shell_single_quote("cd " + shell_single_quote(working_directory.string()) + " && " + command) +
        " > " + shell_single_quote(temp_path);
    log_debug("external command kind=capture spawn command=" + shell_command);
    int result = std::system(shell_command.c_str());
    log_debug("external command kind=capture exit command=" + shell_command + " status=" + std::to_string(result));
    if (result != 0) {
        std::filesystem::remove(temp_path);
        return {};
    }

    std::ifstream input(temp_path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    std::filesystem::remove(temp_path);
    return lines;
#else
    (void)command;
    (void)working_directory;
    return {};
#endif
}

EditorBuffer *find_buffer_by_path(EditorState &state, const std::string &path) {
    for (EditorBuffer &buffer : state.session.buffers()) {
        if (buffer.core.file_path() && *buffer.core.file_path() == path) {
            return &buffer;
        }
    }
    return nullptr;
}

bool parse_grep_selection(
    const std::string &selection,
    std::string &path,
    std::size_t &row,
    std::size_t &column) {
    std::size_t first_colon = selection.find(':');
    if (first_colon == std::string::npos) {
        return false;
    }
    std::size_t second_colon = selection.find(':', first_colon + 1);
    if (second_colon == std::string::npos) {
        return false;
    }
    std::size_t third_colon = selection.find(':', second_colon + 1);
    if (third_colon == std::string::npos) {
        return false;
    }

    path = selection.substr(0, first_colon);
    try {
        std::size_t parsed_row = static_cast<std::size_t>(std::stoul(selection.substr(first_colon + 1, second_colon - first_colon - 1)));
        std::size_t parsed_column =
            static_cast<std::size_t>(std::stoul(selection.substr(second_colon + 1, third_colon - second_colon - 1)));
        row = parsed_row > 0 ? parsed_row - 1 : 0;
        column = parsed_column > 0 ? parsed_column - 1 : 0;
    } catch (const std::exception &) {
        return false;
    }
    return !path.empty();
}

void handle_find_file_command(EditorState &state) {
    std::string error_message;
    std::optional<std::string> selection = run_picker_command(state, "rg --files | fzf", error_message);
    if (!selection) {
        log_debug("find-file canceled/error: " + error_message);
        set_status(state, error_message);
        return;
    }
    std::filesystem::path resolved = *selection;
    if (resolved.is_relative()) {
        resolved = std::filesystem::current_path() / resolved;
    }
    resolved = resolved.lexically_normal();
    log_debug("find-file resolved path=" + resolved.string());
    handle_edit_command(state, resolved.string());
}

void open_startup_file_picker(EditorState &state) {
    std::string error_message;
    std::optional<std::string> selection = run_picker_command(state, "rg --files | fzf", error_message);
    if (!selection) {
        log_debug("startup picker canceled/error: " + error_message);
        if (!error_message.empty()) {
            set_status(state, error_message);
        }
        return;
    }

    std::filesystem::path resolved = *selection;
    if (resolved.is_relative()) {
        resolved = std::filesystem::current_path() / resolved;
    }
    resolved = resolved.lexically_normal();
    log_debug("startup picker resolved path=" + resolved.string());

    EditorBuffer *buffer = state.session.open_file(resolved.string(), true);
    if (!buffer) {
        log_debug("startup picker open failed path=" + resolved.string());
        set_status(state, "Could not open file");
        return;
    }
    state.buffer_ui.try_emplace(buffer->id);
    set_status(state, "Opened " + resolved.string());
}

void handle_grep_command(EditorState &state, const std::string &argument) {
    if (argument.empty()) {
        set_status(state, "No grep pattern");
        return;
    }

    std::string error_message;
    std::string command =
        "rg --column --line-number --no-heading --color=never --smart-case " +
        shell_single_quote(argument) + " | fzf";
    std::optional<std::string> selection = run_picker_command(state, command, error_message);
    if (!selection) {
        log_debug("grep picker canceled/error: " + error_message);
        set_status(state, error_message);
        return;
    }

    std::string path;
    std::size_t row = 0;
    std::size_t column = 0;
    if (!parse_grep_selection(*selection, path, row, column)) {
        log_debug("grep parse failed selection=[" + *selection + "]");
        set_status(state, "Could not parse grep result");
        return;
    }
    std::filesystem::path resolved = path;
    if (resolved.is_relative()) {
        resolved = std::filesystem::current_path() / resolved;
    }
    path = resolved.lexically_normal().string();
    log_debug("grep resolved path=" + path + " row=" + std::to_string(row) + " column=" + std::to_string(column));

    EditorBuffer *existing = find_buffer_by_path(state, path);
    if (existing) {
        state.session.switch_to_id(existing->id);
        active_core(state).set_cursor({row, column});
        set_status(state, "Opened " + path);
        return;
    }

    EditorBuffer *buffer = state.session.open_file(path, true);
    if (!buffer) {
        log_debug("grep open failed path=" + path);
        set_status(state, "Could not open file");
        return;
    }
    state.buffer_ui.try_emplace(buffer->id);
    active_core(state).set_cursor({row, column});
    set_status(state, "Opened " + path);
}

void request_definition(EditorState &state) {
    EditorCore &core = active_core(state);
    ServiceRequest request;
    request.type = ServiceRequestType::GoToDefinition;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position(core.cursor());
    state.runtime.dispatch_service_request(request);
    set_status(state, "Definition requested");
}

std::optional<std::string> token_under_cursor(const EditorCore &core) {
    Position cursor = core.cursor();
    if (cursor.row >= core.line_count()) {
        return std::nullopt;
    }
    const std::u32string &line = core.lines()[cursor.row];
    if (line.empty()) {
        return std::nullopt;
    }

    auto is_path_char = [](char32_t ch) {
        return (ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z') || (ch >= U'0' && ch <= U'9') ||
            ch == U'_' || ch == U'-' || ch == U'.' || ch == U'/' || ch == U'\\';
    };

    std::size_t column = cursor.column;
    if (column >= line.size()) {
        if (column == 0) {
            return std::nullopt;
        }
        column = line.size() - 1;
    } else if (!is_path_char(line[column]) && column > 0 && is_path_char(line[column - 1])) {
        --column;
    }

    if (!is_path_char(line[column])) {
        return std::nullopt;
    }

    std::size_t start = column;
    while (start > 0 && is_path_char(line[start - 1])) {
        --start;
    }
    std::size_t end = column + 1;
    while (end < line.size() && is_path_char(line[end])) {
        ++end;
    }

    std::string token = u32_to_utf8(line.substr(start, end - start));
    while (!token.empty() && (token.front() == '"' || token.front() == '\'')) {
        token.erase(token.begin());
    }
    while (!token.empty() && (token.back() == '"' || token.back() == '\'' || token.back() == ',' || token.back() == ';')) {
        token.pop_back();
    }
    if (token.empty()) {
        return std::nullopt;
    }
    return token;
}

std::filesystem::path workspace_root_for_buffer(const EditorState &state, const EditorCore &core) {
    if (const LspServerConfig *server = matching_lsp_server(state.config, core.file_path())) {
        return infer_workspace_root(*server, core.file_path());
    }
    if (core.file_path()) {
        return std::filesystem::path(*core.file_path()).parent_path();
    }
    return std::filesystem::current_path();
}

std::optional<std::filesystem::path> resolve_direct_file_reference(
    const EditorState &state,
    const EditorCore &core,
    const std::string &token) {
    std::filesystem::path candidate = token;
    std::vector<std::filesystem::path> roots;
    if (candidate.is_absolute()) {
        if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
            return candidate.lexically_normal();
        }
        return std::nullopt;
    }
    if (core.file_path()) {
        roots.push_back(std::filesystem::path(*core.file_path()).parent_path());
    }
    std::filesystem::path workspace_root = workspace_root_for_buffer(state, core);
    if (!workspace_root.empty()) {
        roots.push_back(workspace_root);
    }
    roots.push_back(std::filesystem::current_path());

    for (const std::filesystem::path &root : roots) {
        std::filesystem::path resolved = (root / candidate).lexically_normal();
        if (std::filesystem::exists(resolved) && std::filesystem::is_regular_file(resolved)) {
            return resolved;
        }
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> search_workspace_for_file(
    const EditorState &state,
    const std::filesystem::path &workspace_root,
    const std::string &token) {
    std::optional<std::string> finder = first_available_executable({"fdfind", "fd"});
    if (!finder) {
        return {};
    }

    std::filesystem::path token_path = token;
    std::string pattern = token_path.has_parent_path() ? "*" + token : token_path.filename().string();
    std::string command =
        *finder + " -a -t f -g " + shell_single_quote(pattern) + " " + shell_single_quote(workspace_root.string()) + " 2>/dev/null";
    log_debug("file-under-cursor search root=" + workspace_root.string() + " token=" + token + " pattern=" + pattern);
    std::vector<std::string> lines = run_capture_command(command, std::filesystem::current_path());
    std::vector<std::filesystem::path> matches;
    auto candidate_paths = lines
        | std::views::transform([](const std::string &line) {
              return std::filesystem::path(line).lexically_normal();
          })
        | std::views::filter([](const std::filesystem::path &path) {
              return std::filesystem::exists(path) && std::filesystem::is_regular_file(path);
          });
    for (const std::filesystem::path &path : candidate_paths) {
        matches.push_back(path);
    }
    std::ranges::sort(matches);
    matches.erase(std::ranges::unique(matches).begin(), matches.end());
    return matches;
}

void open_file_under_cursor(EditorState &state) {
    EditorCore &core = active_core(state);
    std::optional<std::string> token = token_under_cursor(core);
    if (!token) {
        set_status(state, "No file reference under cursor");
        return;
    }

    if (std::optional<std::filesystem::path> direct = resolve_direct_file_reference(state, core, *token)) {
        handle_edit_command(state, direct->string());
        return;
    }

    std::filesystem::path workspace_root = workspace_root_for_buffer(state, core);
    std::vector<std::filesystem::path> matches = search_workspace_for_file(state, workspace_root, *token);
    if (matches.empty()) {
        if (!first_available_executable({"fdfind", "fd"})) {
            set_status(state, "Missing executable: fdfind/fd");
        } else {
            set_status(state, "File not found: " + *token);
        }
        return;
    }
    if (matches.size() > 1) {
        set_status(state, "Ambiguous file reference: " + *token);
        return;
    }
    handle_edit_command(state, matches.front().string());
}

EditorState::JumpLocation current_jump_location(const EditorState &state) {
    const EditorCore &core = active_core(state);
    return {core.document_uri(), core.file_path(), core.cursor()};
}

bool same_jump_location(const EditorState::JumpLocation &left, const EditorState::JumpLocation &right) {
    return left.document_uri == right.document_uri && positions_equal(left.position, right.position);
}

bool open_jump_location(EditorState &state, const EditorState::JumpLocation &location) {
    EditorBuffer *buffer = state.session.find_buffer_by_uri(location.document_uri);
    if (!buffer) {
        std::string path;
        if (location.file_path && !location.file_path->empty()) {
            path = *location.file_path;
        } else {
            path = file_path_from_uri(location.document_uri);
        }
        if (path.empty()) {
            return false;
        }
        buffer = state.session.open_file(path, true);
        if (!buffer) {
            return false;
        }
        state.buffer_ui.try_emplace(buffer->id);
    } else {
        state.session.switch_to_id(buffer->id);
    }
    buffer->core.set_cursor(location.position);
    return true;
}

void navigate_jump_history(
    EditorState &state,
    std::vector<EditorState::JumpLocation> &from_stack,
    std::vector<EditorState::JumpLocation> &to_stack,
    const std::string &empty_message,
    const std::string &success_message) {
    if (from_stack.empty()) {
        set_status(state, empty_message);
        return;
    }

    EditorState::JumpLocation target = from_stack.back();
    from_stack.pop_back();
    EditorState::JumpLocation current = current_jump_location(state);
    if (!open_jump_location(state, target)) {
        from_stack.push_back(std::move(target));
        set_status(state, "Jump target unavailable");
        return;
    }
    if (to_stack.empty() || !same_jump_location(to_stack.back(), current)) {
        to_stack.push_back(std::move(current));
    }
    set_status(state, success_message);
}

bool reload_editor_configuration(EditorState &state, std::string &error_message) {
    try {
        EditorConfig new_config = load_editor_config();
        KeyBindings new_keybindings = load_keybindings(new_config);
        Theme new_theme = load_theme(new_config);

        state.config = std::move(new_config);
        configure_logger(state.config.log_path);
        log_debug("reload-config applied");
        state.session.configure_clipboard(state.config.clipboard);
        state.keybindings = std::move(new_keybindings);
        state.theme = std::move(new_theme);
        invalidate_syntax_runtime_cache();
        invalidate_syntax_cache(state);
        apply_theme_to_terminal(state.theme);
        clearok(stdscr, TRUE);
        refresh();
        state.runtime.clear_services();
        if (!state.config.lsp_servers.empty()) {
            for (const LspServerConfig &server : state.config.lsp_servers) {
                state.runtime.add_service(std::make_unique<LspService>(server));
            }
        }
        if (!state.config.lsp_servers.empty()) {
            state.runtime.start_services();
        }
        return true;
    } catch (const std::exception &error) {
        error_message = error.what();
        return false;
    }
}

void execute_filter_command(EditorState &state) {
    EditorCore &core = active_core(state);
    std::optional<Range> selection = core.selection_range();
    if (!selection) {
        set_status(state, "No selection");
        enter_normal_mode(state);
        return;
    }

    std::string command = u32_to_utf8(state.command_buffer);
    if (command.empty()) {
        set_status(state, "No filter command");
        enter_normal_mode(state);
        return;
    }

    std::u32string output_text;
    std::string error_message;
    if (!run_selection_filter_command(state, command, core.read_text(*selection), output_text, error_message)) {
        set_status(state, error_message);
        enter_normal_mode(state);
        return;
    }

    if (core.selection_mode() == SelectionMode::Line && !output_text.empty() && output_text.back() != U'\n') {
        output_text.push_back(U'\n');
    }

    core.replace_range(*selection, output_text);
    enter_normal_mode(state);
    set_status(state, "Selection filtered");
}

void execute_sed_command(EditorState &state) {
    EditorCore &core = active_core(state);
    std::optional<Range> selection = core.selection_range();
    if (!selection) {
        set_status(state, "No selection");
        enter_normal_mode(state);
        return;
    }

    std::string script = u32_to_utf8(state.command_buffer);
    if (script.empty()) {
        set_status(state, "No sed command");
        enter_normal_mode(state);
        return;
    }

    std::u32string output_text;
    std::string error_message;
    if (!run_selection_filter_command(
            state,
            "sed " + script,
            core.read_text(*selection),
            output_text,
            error_message)) {
        set_status(state, error_message);
        enter_normal_mode(state);
        return;
    }

    if (core.selection_mode() == SelectionMode::Line && !output_text.empty() && output_text.back() != U'\n') {
        output_text.push_back(U'\n');
    }

    core.replace_range(*selection, output_text);
    enter_normal_mode(state);
    set_status(state, "Selection filtered with sed");
}

void execute_command(EditorState &state) {
    if (state.command_prompt_kind == CommandPromptKind::FilterSelection) {
        execute_filter_command(state);
        return;
    }
    if (state.command_prompt_kind == CommandPromptKind::SedSelection) {
        execute_sed_command(state);
        return;
    }

    std::u32string command_text = state.command_buffer;
    std::string command = u32_to_utf8(command_text);
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
    add_prompt_history_entry(state, command_text);
    if (std::ranges::all_of(verb, [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        handle_goto_line_command(state, verb);
    } else if (verb == "w") {
        handle_write_command(state, argument);
    } else if (verb == "q") {
        handle_quit_command(state, false);
    } else if (verb == "q!") {
        handle_quit_command(state, true);
    } else if (verb == "wq" || verb == "x") {
        handle_write_quit_command(state, argument);
    } else if (verb == "e") {
        handle_edit_command(state, argument);
    } else if (verb == "buffers") {
        set_status(state, buffers_summary(state));
    } else if (verb == "buffer") {
        handle_buffer_switch_command(state, argument);
    } else if (verb == "bnext") {
        state.session.next_buffer();
        set_status(state, std::format("Switched to {}", active_core(state).display_file_name()));
    } else if (verb == "bprev") {
        state.session.previous_buffer();
        set_status(state, std::format("Switched to {}", active_core(state).display_file_name()));
    } else if (verb == "bd") {
        handle_buffer_delete_command(state, false);
    } else if (verb == "bd!") {
        handle_buffer_delete_command(state, true);
    } else if (verb == "find-file") {
        handle_find_file_command(state);
    } else if (verb == "grep") {
        handle_grep_command(state, argument);
    } else if (verb == "reload-config") {
        std::string error_message;
        if (reload_editor_configuration(state, error_message)) {
            set_status(state, "Reloaded config");
        } else {
            set_status(state, "Config reload failed: " + error_message);
        }
    } else if (verb == "diagnostics") {
        show_diagnostics_summary(state);
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
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    const EditorCore &core = active_core(state);
    buffer_ui.search_matches.clear();
    buffer_ui.current_search_match_index.reset();
    buffer_ui.search_pattern_valid = true;
    buffer_ui.search_matches_version = core.document_version();

    if (buffer_ui.active_search_pattern.empty()) {
        buffer_ui.compiled_search_pattern_utf8.clear();
        buffer_ui.compiled_search_regex.reset();
        return;
    }

    try {
        buffer_ui.compiled_search_pattern_utf8 = u32_to_utf8(buffer_ui.active_search_pattern);
        buffer_ui.compiled_search_regex = std::make_unique<std::regex>(
            buffer_ui.compiled_search_pattern_utf8,
            std::regex::ECMAScript | std::regex::optimize);

        for (std::size_t row = 0; row < core.line_count(); ++row) {
            const std::u32string &line = core.lines()[row];
            std::string line_utf8 = u32_to_utf8(line);
            for (std::sregex_iterator it(line_utf8.begin(), line_utf8.end(), *buffer_ui.compiled_search_regex), end;
                 it != end;
                 ++it) {
                if (it->length() == 0) {
                    continue;
                }
                std::size_t start_offset = static_cast<std::size_t>(it->position());
                std::size_t end_offset = start_offset + static_cast<std::size_t>(it->length());
                buffer_ui.search_matches.push_back(
                    {{row, column_for_utf8_offset_in_line(line, start_offset)},
                     {row, column_for_utf8_offset_in_line(line, end_offset)}});
            }
        }
    } catch (const std::regex_error &) {
        buffer_ui.search_pattern_valid = false;
        buffer_ui.compiled_search_regex.reset();
    }
}

void refresh_search_matches(EditorState &state, bool move_to_best_match) {
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    EditorCore &core = active_core(state);
    if (buffer_ui.search_matches_version != core.document_version() ||
        buffer_ui.compiled_search_pattern_utf8 != u32_to_utf8(buffer_ui.active_search_pattern)) {
        rebuild_search_matches(state);
    }

    if (!buffer_ui.search_pattern_valid || buffer_ui.search_matches.empty()) {
        if (move_to_best_match) {
            buffer_ui.current_search_match_index.reset();
        }
        return;
    }

    Position anchor = move_to_best_match ? buffer_ui.search_origin : core.cursor();
    std::size_t best_index = 0;
    bool found_best = false;
    for (std::size_t index = 0; index < buffer_ui.search_matches.size(); ++index) {
        if (!position_less_than(buffer_ui.search_matches[index].start, anchor)) {
            best_index = index;
            found_best = true;
            break;
        }
    }
    buffer_ui.current_search_match_index = found_best ? best_index : 0;
    if (move_to_best_match) {
        core.set_cursor(buffer_ui.search_matches[*buffer_ui.current_search_match_index].start);
    }
}

void navigate_search_match(EditorState &state, bool forward) {
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    EditorCore &core = active_core(state);
    if (buffer_ui.search_matches.empty()) {
        set_status(state, buffer_ui.search_pattern_valid ? "No search matches" : "invalid regex");
        return;
    }

    std::size_t index = buffer_ui.current_search_match_index.value_or(0);
    if (forward) {
        index = (index + 1) % buffer_ui.search_matches.size();
    } else {
        index = index == 0 ? buffer_ui.search_matches.size() - 1 : index - 1;
    }
    buffer_ui.current_search_match_index = index;
    core.set_cursor(buffer_ui.search_matches[index].start);
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

std::size_t display_width(const std::u32string &line) {
    return display_width_until(line, line.size());
}

void ensure_horizontal_visibility(EditorState &state, int screen_cols) {
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    const EditorCore &core = active_core(state);
    Position cursor = core.cursor();
    const std::u32string &line = core.lines()[cursor.row];
    std::size_t cursor_x = display_width_until(line, cursor.column);
    std::size_t usable_cols = screen_cols > 0 ? static_cast<std::size_t>(screen_cols) : 1;

    while (buffer_ui.col_offset > cursor_x) {
        --buffer_ui.col_offset;
    }
    while (cursor_x >= buffer_ui.col_offset + usable_cols) {
        ++buffer_ui.col_offset;
    }
}

void ensure_vertical_visibility(EditorState &state, int screen_rows, int buffer_cols) {
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    std::size_t usable_rows = screen_rows > 0 ? static_cast<std::size_t>(screen_rows) : 1;
    std::vector<VisualRow> visual_rows = build_visual_rows(state, buffer_cols);
    std::size_t cursor_visual_row = visual_row_for_buffer_row(visual_rows, active_core(state).cursor().row);
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

void ensure_cursor_visible(EditorState &state, int buffer_rows, int buffer_cols) {
    ensure_vertical_visibility(state, buffer_rows, buffer_cols);
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
    const EditorCore &core = active_core(state);
    const EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    std::vector<HighlightSpan> spans;
    Range entire_line = core.line_range(row);
    if (row < buffer_ui.syntax_highlights.size()) {
        spans.insert(spans.end(), buffer_ui.syntax_highlights[row].begin(), buffer_ui.syntax_highlights[row].end());
    }
    if (core.cursor().row == row) {
        spans.push_back({entire_line, StyleRole::CursorLine, 10});
    }
    std::optional<Range> selection = core.selection_range();
    if (selection && row >= selection->start.row && row <= selection->end.row) {
        Range line_selection{
            {row, row == selection->start.row ? selection->start.column : 0},
            {row, row == selection->end.row ? selection->end.column : core.line_length(row)}};
        if (!positions_equal(line_selection.start, line_selection.end)) {
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
        if (positions_equal(line_match.start, line_match.end)) {
            continue;
        }
        StyleRole role =
            buffer_ui.current_search_match_index && *buffer_ui.current_search_match_index == index
                ? StyleRole::SearchMatchCurrent
                : StyleRole::SearchMatch;
        int priority = role == StyleRole::SearchMatchCurrent ? 95 : 90;
        spans.push_back({line_match, role, priority});
    }
    for (const Diagnostic &diagnostic : core.diagnostics()) {
        Range range = normalized_range(diagnostic.range);
        if (row < range.start.row || row > range.end.row) {
            continue;
        }
        Range line_diagnostic{
            {row, row == range.start.row ? range.start.column : 0},
            {row, row == range.end.row ? range.end.column : core.line_length(row)}};
        if (positions_equal(line_diagnostic.start, line_diagnostic.end)) {
            continue;
        }
        StyleRole role = diagnostic.severity == DiagnosticSeverity::Error
                             ? StyleRole::DiagnosticError
                             : StyleRole::DiagnosticWarning;
        spans.push_back({line_diagnostic, role, 80});
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

StyleRole annotation_role(const InlineAnnotation &annotation) {
    switch (annotation.severity) {
        case AnnotationSeverity::Error:
            return StyleRole::DiagnosticError;
        case AnnotationSeverity::Warning:
            return StyleRole::DiagnosticWarning;
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

void draw_buffer_rows(const EditorState &state, int buffer_rows, int buffer_cols, int line_number_width) {
    const EditorCore &core = active_core(state);
    const EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    std::vector<VisualRow> visual_rows = build_visual_rows(state, buffer_cols);
    for (int screen_row = 0; screen_row < buffer_rows; ++screen_row) {
        std::size_t visual_row_index = buffer_ui.row_offset + static_cast<std::size_t>(screen_row);
        move(screen_row, 0);
        clrtoeol();
        if (visual_row_index >= visual_rows.size()) {
            mvaddch(screen_row, 0, '~');
            continue;
        }

        const VisualRow &visual_row = visual_rows[visual_row_index];
        if (visual_row.kind == VisualRowKind::SourceLine) {
            StyleRole line_number_role =
                core.cursor().row == visual_row.buffer_row ? StyleRole::CursorLineNumber : StyleRole::LineNumber;
            draw_line_number(state.theme, screen_row, visual_row.buffer_row + 1, line_number_width, line_number_role);
            std::vector<HighlightSpan> spans = collect_line_highlights(state, visual_row.buffer_row);
            draw_styled_line(
                state.theme,
                screen_row,
                line_number_width + 1,
                core.lines()[visual_row.buffer_row],
                spans,
                visual_row.buffer_row,
                buffer_ui.col_offset,
                buffer_cols);
            continue;
        }

        const AnnotationEntryView &annotation = *visual_row.annotation;
        StyleRole role =
            annotation.diagnostic_index && buffer_ui.selected_diagnostic_index &&
                    *annotation.diagnostic_index == *buffer_ui.selected_diagnostic_index
                ? StyleRole::StatusBar
                : annotation_role(annotation.annotation);
        TextStyle style = theme_style(state.theme, role);
        attron(curses_attributes(style, role));
        mvprintw(screen_row, 0, "%*s ", line_number_width, ">");
        std::vector<std::u32string> wrapped =
            wrap_annotation_text(annotation_prefix(annotation.annotation) + annotation.annotation.text, buffer_cols - 2);
        std::u32string text = visual_row.wrap_offset < wrapped.size() ? wrapped[visual_row.wrap_offset] : U"";
        int annotation_col = line_number_width + 1;
        if (state.config.right_justify_diagnostics && annotation.annotation.kind == AnnotationKind::Diagnostic) {
            int rendered_width = static_cast<int>(display_width(text));
            annotation_col += std::max(0, buffer_cols - rendered_width);
        }
        mvaddnwstr(screen_row, annotation_col, u32_to_wstring(text).c_str(), buffer_cols);
        attroff(curses_attributes(style, role));
    }
}

std::string build_status_text(const EditorState &state) {
    const EditorCore &core = active_core(state);
    Position cursor = core.cursor();
    std::string language = infer_language_id(state.config, core.file_path());
    std::string workspace = "-";
    if (const LspServerConfig *server = matching_lsp_server(state.config, core.file_path())) {
        std::filesystem::path workspace_root = infer_workspace_root(*server, core.file_path());
        if (!workspace_root.empty()) {
            workspace = workspace_root.string();
        }
    }

    std::string left_text = std::format(
        "{}  {}  [{}/{}]  {}  ws:{}",
        mode_name(state.mode),
        core.display_file_name(),
        state.session.active_buffer_index() + 1,
        state.session.buffer_count(),
        language,
        workspace);
    std::string right_text = std::format(
        "{}  {}:{}  rev {}",
        core.is_dirty() ? " [+]" : "",
        cursor.row + 1,
        cursor.column + 1,
        core.current_revision());
    return left_text + right_text;
}

void draw_status_bar(const EditorState &state, int screen_rows, int screen_cols) {
    TextStyle style = theme_style(state.theme, StyleRole::StatusBar);
    attron(curses_attributes(style, StyleRole::StatusBar));
    move(screen_rows - 2, 0);
    clrtoeol();
    const EditorCore &core = active_core(state);
    Position cursor = core.cursor();
    std::string language = infer_language_id(state.config, core.file_path());
    std::string workspace = "-";
    if (const LspServerConfig *server = matching_lsp_server(state.config, core.file_path())) {
        std::filesystem::path workspace_root = infer_workspace_root(*server, core.file_path());
        if (!workspace_root.empty()) {
            workspace = workspace_root.string();
        }
    }
    std::string left_text = std::format(
        "{}  {}  [{}/{}]  {}  ws:{}",
        mode_name(state.mode),
        core.display_file_name(),
        state.session.active_buffer_index() + 1,
        state.session.buffer_count(),
        language,
        workspace);
    std::string right_text = std::format(
        "{}  {}:{}  rev {}",
        core.is_dirty() ? " [+]" : "",
        cursor.row + 1,
        cursor.column + 1,
        core.current_revision());
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
            prompt = "|";
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

int line_number_width(const EditorState &state) {
    std::size_t line_count = active_core(state).line_count();
    int width = 1;
    while (line_count >= 10) {
        line_count /= 10;
        ++width;
    }
    return width;
}

std::pair<int, int> cursor_screen_position(const EditorState &state, int line_number_cols) {
    const EditorCore &core = active_core(state);
    const EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);
    (void)screen_rows;
    int buffer_cols = screen_cols - line_number_cols - 1;
    std::vector<VisualRow> visual_rows = build_visual_rows(state, buffer_cols);
    std::size_t visual_row_index = visual_row_for_buffer_row(visual_rows, core.cursor().row);
    Position cursor = core.cursor();
    const std::u32string &line = core.lines()[cursor.row];
    std::size_t width = display_width_until(line, cursor.column);
    int screen_row = static_cast<int>(visual_row_index - buffer_ui.row_offset);
    int screen_col = static_cast<int>(width - buffer_ui.col_offset) + line_number_cols + 1;
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
    const EditorCore &core = active_core(state);
    const EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    int total_rows = 0;
    int total_cols = 0;
    getmaxyx(stdscr, total_rows, total_cols);
    int buffer_rows = total_rows - 2;
    if (screen_row < 0 || screen_row >= buffer_rows) {
        return std::nullopt;
    }

    int line_number_cols = line_number_width(state);
    int buffer_cols = total_cols - line_number_cols - 1;
    std::vector<VisualRow> visual_rows = build_visual_rows(state, buffer_cols);
    std::size_t visual_row_index = buffer_ui.row_offset + static_cast<std::size_t>(screen_row);
    if (visual_row_index >= visual_rows.size()) {
        return std::nullopt;
    }
    const VisualRow &visual_row = visual_rows[visual_row_index];
    std::size_t row = visual_row.buffer_row;

    int gutter_start = line_number_cols + 1;
    std::size_t visual_column = buffer_ui.col_offset;
    if (screen_col > gutter_start) {
        visual_column += static_cast<std::size_t>(screen_col - gutter_start);
    }

    const std::u32string &line = core.lines()[row];
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

void refresh_syntax_highlights(EditorState &state) {
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    EditorCore &core = active_core(state);
    SyntaxSelection selection;
    try {
        selection = resolve_syntax_selection(state.config, core.file_path());
    } catch (const std::exception &error) {
        if (!buffer_ui.syntax_config_error_reported) {
            set_status(state, std::string("Syntax config error: ") + error.what());
            buffer_ui.syntax_config_error_reported = true;
        }
        selection = {};
    }

    if (buffer_ui.syntax_revision == core.current_revision() && buffer_ui.syntax_selection == selection &&
        buffer_ui.syntax_file_path == core.file_path()) {
        return;
    }

    buffer_ui.syntax_selection = selection;
    buffer_ui.syntax_file_path = core.file_path();
    buffer_ui.syntax_revision = core.current_revision();
    auto syntax_result = highlight_document_syntax(core.lines(), state.config, buffer_ui.syntax_selection);
    if (!syntax_result) {
        buffer_ui.syntax_highlights.assign(core.lines().size(), {});
        if (!buffer_ui.syntax_config_error_reported) {
            set_status(state, "Syntax error: " + syntax_result.error());
            buffer_ui.syntax_config_error_reported = true;
        }
    } else {
        buffer_ui.syntax_highlights = std::move(*syntax_result);
        buffer_ui.syntax_config_error_reported = false;
    }
}

void append_after_cursor(EditorState &state) {
    EditorCore &core = active_core(state);
    Position cursor = core.cursor();
    std::size_t length = core.line_length(cursor.row);
    if (cursor.column < length) {
        core.set_cursor({cursor.row, cursor.column + 1});
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
    active_core(state).move_by_lines(-page_step());
}

void page_down(EditorState &state) {
    active_core(state).move_by_lines(page_step());
}

void half_page_up(EditorState &state) {
    active_core(state).move_by_lines(-half_page_step());
}

void half_page_down(EditorState &state) {
    active_core(state).move_by_lines(half_page_step());
}

void enter_visual_mode(EditorState &state) {
    state.mode = Mode::Visual;
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    active_core(state).begin_selection(SelectionMode::Character);
    set_status(state, mode_name(state.mode));
}

void enter_visual_line_mode(EditorState &state) {
    state.mode = Mode::VisualLine;
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    active_core(state).begin_selection(SelectionMode::Line);
    set_status(state, mode_name(state.mode));
}

bool select_entire_buffer(EditorState &state) {
    EditorCore &core = active_core(state);
    if (core.line_count() == 0) {
        return false;
    }

    std::size_t last_row = core.line_count() - 1;
    Range whole_buffer{{0, 0}, {last_row, core.line_length(last_row)}};
    SelectionMode selection_mode = state.mode == Mode::VisualLine ? SelectionMode::Line : SelectionMode::Character;
    if (!core.set_selection_range(whole_buffer, selection_mode)) {
        return false;
    }

    state.mode = selection_mode == SelectionMode::Line ? Mode::VisualLine : Mode::Visual;
    set_status(state, mode_name(state.mode));
    return true;
}

std::pair<std::size_t, std::size_t> selected_line_span(const EditorCore &core) {
    if (std::optional<Range> selection = core.selection_range()) {
        Range normalized = normalized_range(*selection);
        return {normalized.start.row, normalized.end.row > normalized.start.row ? normalized.end.row - (normalized.end.column == 0 ? 1 : 0) : normalized.start.row};
    }
    std::size_t row = core.cursor().row;
    return {row, row};
}

bool indent_selection_or_line(EditorState &state, bool indent) {
    EditorCore &core = active_core(state);
    auto [start_row, end_row] = selected_line_span(core);
    bool changed = indent
        ? core.indent_lines(start_row, end_row, state.config.shiftwidth)
        : core.outdent_lines(start_row, end_row, state.config.shiftwidth);
    if (changed && (state.mode == Mode::Visual || state.mode == Mode::VisualLine)) {
        state.mode = Mode::Normal;
    }
    return changed;
}

bool motion_is_character_based(PendingMotion motion) {
    return motion != PendingMotion::None;
}

bool execute_pending_motion(EditorState &state, char32_t target) {
    EditorCore &core = active_core(state);
    bool moved = false;
    for (std::size_t attempt = 0; attempt < state.pending_motion_repeat_count; ++attempt) {
        bool step_moved = false;
        switch (state.pending_motion) {
            case PendingMotion::FindForward:
                step_moved = core.move_to_character_forward(target, true);
                break;
            case PendingMotion::FindBackward:
                step_moved = core.move_to_character_backward(target, true);
                break;
            case PendingMotion::TillForward:
                step_moved = core.move_to_character_forward(target, false);
                break;
            case PendingMotion::TillBackward:
                step_moved = core.move_to_character_backward(target, false);
                break;
            case PendingMotion::None:
                return false;
        }
        if (!step_moved) {
            break;
        }
        moved = true;
    }
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.pending_tokens.clear();
    set_status(state, moved ? mode_name(state.mode) : "Target not found");
    return moved;
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
            case KEY_BTAB:
                return "shift-tab";
            case KEY_BACKSPACE:
                return "backspace";
            default:
                return std::nullopt;
        }
    }

    if (key == 27) {
        return "esc";
    }
    if (key == '\t') {
        return "tab";
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

bool mode_supports_command_language(const EditorState &state) {
    return state.mode == Mode::Normal || state.mode == Mode::Visual || state.mode == Mode::VisualLine;
}

bool token_is_digit(const std::string &token) {
    return token.size() == 1 && token[0] >= '0' && token[0] <= '9';
}

bool token_starts_repeat(const EditorState &state, const std::string &token) {
    if (!mode_supports_command_language(state) || !token_is_digit(token)) {
        return false;
    }
    if (!state.repeat_digits.empty()) {
        return true;
    }
    return token != "0";
}

std::size_t current_repeat_count(const EditorState &state) {
    if (state.repeat_digits.empty()) {
        return 1;
    }
    std::size_t repeat = 0;
    for (char ch : state.repeat_digits) {
        repeat = repeat * 10 + static_cast<std::size_t>(ch - '0');
    }
    return repeat == 0 ? 1 : repeat;
}

std::size_t take_repeat_count(EditorState &state) {
    std::size_t repeat = current_repeat_count(state);
    state.repeat_digits.clear();
    return repeat;
}

void record_group_input(EditorState &state, const std::string &token, wint_t key, bool printable) {
    if (state.group_depth == 0) {
        return;
    }
    state.group_inputs.push_back({token, key, printable});
}

std::optional<wint_t> key_from_token(const std::string &token) {
    if (token == "enter") {
        return '\n';
    }
    if (token == "backspace") {
        return KEY_BACKSPACE;
    }
    if (token == "esc") {
        return 27;
    }
    if (token.starts_with("ctrl-") && token.size() == 6) {
        char ch = token[5];
        if (ch >= 'a' && ch <= 'z') {
            return static_cast<wint_t>(ch - 'a' + 1);
        }
    }
    std::u32string text = utf8_to_u32(token);
    if (text.size() == 1) {
        return static_cast<wint_t>(text[0]);
    }
    return std::nullopt;
}

bool token_is_printable_for_replay(const std::string &token) {
    if (token == "esc" || token == "enter" || token == "backspace" || token == "left" || token == "right" ||
        token == "up" || token == "down" || token == "pageup" || token == "pagedown" || token == "tab" ||
        token == "shift-tab") {
        return false;
    }
    if (token.starts_with("ctrl-")) {
        return false;
    }
    return true;
}

void execute_dispatch(EditorState &state, const KeyDispatch &dispatch, wint_t key);
void process_input_token(EditorState &state, const std::string &token, wint_t key, bool printable);

void execute_expansion(EditorState &state, const std::vector<std::string> &expansion) {
    static constexpr std::size_t kMaxExpansionDepth = 16;
    if (state.replay_depth >= kMaxExpansionDepth) {
        set_status(state, "Keybinding expansion too deep");
        state.pending_tokens.clear();
        return;
    }

    ++state.replay_depth;
    for (const std::string &token : expansion) {
        std::optional<wint_t> key = key_from_token(token);
        process_input_token(state, token, key.value_or(0), token_is_printable_for_replay(token));
    }
    --state.replay_depth;
}

bool action_accepts_repeat(EditorAction action) {
    switch (action) {
        case EditorAction::MoveLeft:
        case EditorAction::MoveRight:
        case EditorAction::MoveUp:
        case EditorAction::MoveDown:
        case EditorAction::MoveLineStart:
        case EditorAction::MoveLineEnd:
        case EditorAction::DeleteChar:
        case EditorAction::Undo:
        case EditorAction::Redo:
        case EditorAction::PasteAfter:
        case EditorAction::PasteBefore:
        case EditorAction::GotoTop:
        case EditorAction::GotoBottom:
        case EditorAction::DeleteLine:
        case EditorAction::HalfPageDown:
        case EditorAction::HalfPageUp:
        case EditorAction::PageUp:
        case EditorAction::PageDown:
        case EditorAction::Indent:
        case EditorAction::Outdent:
        case EditorAction::SearchNext:
        case EditorAction::SearchPrevious:
        case EditorAction::NextDiagnostic:
        case EditorAction::PreviousDiagnostic:
            return true;
        default:
            return false;
    }
}

bool action_defers_completion(EditorAction action) {
    return action == EditorAction::FindForward || action == EditorAction::FindBackward ||
        action == EditorAction::TillForward || action == EditorAction::TillBackward;
}

void replay_group_inputs(EditorState &state, const std::vector<EditorState::RecordedInput> &inputs, std::size_t repeat) {
    static constexpr std::size_t kMaxReplayDepth = 16;
    if (repeat <= 1) {
        return;
    }
    if (state.replay_depth >= kMaxReplayDepth) {
        set_status(state, "Command group nesting too deep");
        return;
    }

    ++state.replay_depth;
    for (std::size_t iteration = 1; iteration < repeat; ++iteration) {
        for (const EditorState::RecordedInput &input : inputs) {
            process_input_token(state, input.token, input.key, input.printable);
        }
    }
    --state.replay_depth;
}

void execute_action(EditorState &state, EditorAction action, wint_t key) {
    EditorCore &core = active_core(state);
    EditorState::BufferUiState &buffer_ui = active_buffer_ui(state);
    switch (action) {
        case EditorAction::MoveLeft:
            core.move_left();
            break;
        case EditorAction::MoveRight:
            core.move_right();
            break;
        case EditorAction::MoveUp:
            core.move_up();
            break;
        case EditorAction::MoveDown:
            core.move_down();
            break;
        case EditorAction::MoveLineStart:
            core.move_line_start();
            break;
        case EditorAction::MoveLineEnd:
            core.move_line_end();
            break;
        case EditorAction::FindForward:
            state.pending_motion_repeat_count = take_repeat_count(state);
            state.pending_motion = PendingMotion::FindForward;
            set_status(state, "f");
            break;
        case EditorAction::FindBackward:
            state.pending_motion_repeat_count = take_repeat_count(state);
            state.pending_motion = PendingMotion::FindBackward;
            set_status(state, "F");
            break;
        case EditorAction::TillForward:
            state.pending_motion_repeat_count = take_repeat_count(state);
            state.pending_motion = PendingMotion::TillForward;
            set_status(state, "t");
            break;
        case EditorAction::TillBackward:
            state.pending_motion_repeat_count = take_repeat_count(state);
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
            Position cursor = core.cursor();
            core.set_cursor({cursor.row, core.line_length(cursor.row)});
            enter_insert_mode(state);
            break;
        }
        case EditorAction::InsertLineStartInsert: {
            Position cursor = core.cursor();
            core.set_cursor({cursor.row, 0});
            enter_insert_mode(state);
            break;
        }
        case EditorAction::OpenLineBelow:
            core.open_line_below();
            state.mode = Mode::Insert;
            set_status(state, mode_name(state.mode));
            break;
        case EditorAction::OpenLineAbove:
            core.open_line_above();
            state.mode = Mode::Insert;
            set_status(state, mode_name(state.mode));
            break;
        case EditorAction::DeleteChar:
            core.delete_character_under_cursor();
            set_status(state, "Deleted character");
            break;
        case EditorAction::Undo:
            set_status(state, core.undo() ? "Undid change" : "Nothing to undo");
            break;
        case EditorAction::Redo:
            set_status(state, core.redo() ? "Redid change" : "Nothing to redo");
            break;
        case EditorAction::PasteAfter:
            state.session.sync_active_clipboard();
            set_status(state, core.paste_after_cursor() ? "Pasted" : "Yank buffer empty");
            break;
        case EditorAction::PasteBefore:
            state.session.sync_active_clipboard();
            set_status(state, core.paste_before_cursor() ? "Pasted" : "Yank buffer empty");
            break;
        case EditorAction::GotoTop:
            core.move_to_first_line();
            core.move_line_start();
            set_status(state, "Top of file");
            break;
        case EditorAction::GotoBottom:
            core.move_to_last_line();
            core.move_line_start();
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
            core.delete_current_line();
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
        case EditorAction::Indent:
            set_status(state, indent_selection_or_line(state, true) ? "Indented" : "Nothing to indent");
            break;
        case EditorAction::Outdent:
            set_status(state, indent_selection_or_line(state, false) ? "Outdented" : "Nothing to outdent");
            break;
        case EditorAction::NextBuffer:
            state.session.next_buffer();
            set_status(state, "Switched to " + active_core(state).display_file_name());
            break;
        case EditorAction::PreviousBuffer:
            state.session.previous_buffer();
            set_status(state, "Switched to " + active_core(state).display_file_name());
            break;
        case EditorAction::Suspend:
            suspend_editor(state);
            break;
        case EditorAction::EnterNormalMode:
            enter_normal_mode(state);
            break;
        case EditorAction::InsertNewline:
            core.insert_newline();
            break;
        case EditorAction::Backspace:
            core.backspace_character();
            break;
        case EditorAction::CommandExecute:
            execute_command(state);
            break;
        case EditorAction::CommandBackspace:
            if (!state.command_buffer.empty()) {
                state.command_buffer.pop_back();
            }
            break;
        case EditorAction::CommandHistoryPrevious:
            browse_prompt_history(state, true);
            break;
        case EditorAction::CommandHistoryNext:
            browse_prompt_history(state, false);
            break;
        case EditorAction::SelfInsert:
            core.insert_codepoint(static_cast<char32_t>(key));
            break;
        case EditorAction::CommandInsert:
            state.command_buffer.push_back(static_cast<char32_t>(key));
            break;
        case EditorAction::SearchExecute:
            buffer_ui.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            if (!buffer_ui.search_pattern_valid) {
                set_search_status(state, "invalid regex");
                break;
            }
            state.mode = Mode::Normal;
            set_status(state, buffer_ui.search_matches.empty() ? "No search matches" : "Search applied");
            break;
        case EditorAction::SearchBackspace:
            if (!state.search_buffer.empty()) {
                state.search_buffer.pop_back();
            }
            buffer_ui.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            set_search_status(state, buffer_ui.search_pattern_valid ? "" : "invalid regex");
            break;
        case EditorAction::SearchInsert:
            state.search_buffer.push_back(static_cast<char32_t>(key));
            buffer_ui.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            set_search_status(state, buffer_ui.search_pattern_valid ? "" : "invalid regex");
            break;
        case EditorAction::SearchNext:
            refresh_search_matches(state, false);
            navigate_search_match(state, true);
            break;
        case EditorAction::SearchPrevious:
            refresh_search_matches(state, false);
            navigate_search_match(state, false);
            break;
        case EditorAction::GoToDefinition:
            request_definition(state);
            break;
        case EditorAction::GoToFileUnderCursor:
            open_file_under_cursor(state);
            break;
        case EditorAction::JumpBack:
            navigate_jump_history(state, state.jump_back_stack, state.jump_forward_stack, "No older jump", "Jumped back");
            break;
        case EditorAction::JumpForward:
            navigate_jump_history(state, state.jump_forward_stack, state.jump_back_stack, "No newer jump", "Jumped forward");
            break;
        case EditorAction::NextDiagnostic:
            navigate_diagnostic(state, true);
            break;
        case EditorAction::PreviousDiagnostic:
            navigate_diagnostic(state, false);
            break;
        case EditorAction::ToggleDiagnosticsPanel:
            show_diagnostics_summary(state);
            break;
        case EditorAction::DeleteSelection:
            {
            SelectionMode selection_mode = core.selection_mode();
            if (core.delete_selection()) {
                state.session.capture_active_clipboard();
                state.mode = Mode::Normal;
                set_status(
                    state,
                    selection_mode == SelectionMode::Line ? "Deleted lines" : "Deleted selection");
            } else {
                set_status(state, "No selection");
            }
            break;
            }
        case EditorAction::FilterSelection:
            enter_filter_command_mode(state);
            break;
        case EditorAction::SedSelection:
            enter_sed_command_mode(state);
            break;
        case EditorAction::YankSelection:
            if (core.yank_selection()) {
                SelectionMode selection_mode = core.selection_mode();
                state.session.capture_active_clipboard();
                core.clear_selection();
                state.mode = Mode::Normal;
                set_status(state, selection_mode == SelectionMode::Line ? "Yanked lines" : "Yanked selection");
            } else {
                set_status(state, "No selection");
            }
            break;
        case EditorAction::ChangeSelection:
            {
            if (core.delete_selection()) {
                state.session.capture_active_clipboard();
                enter_insert_mode(state);
            } else {
                set_status(state, "No selection");
            }
            break;
            }
        case EditorAction::ReplaceSelectionWithYank:
            {
            state.session.sync_active_clipboard();
            SelectionMode selection_mode = core.selection_mode();
            if (core.replace_selection_with_yank()) {
                state.mode = Mode::Normal;
                set_status(
                    state,
                    selection_mode == SelectionMode::Line ? "Replaced lines" : "Replaced selection");
            } else {
                set_status(state, "Yank buffer empty");
            }
            break;
            }
        case EditorAction::SelectAll:
            if (!select_entire_buffer(state)) {
                set_status(state, "Selection empty");
            }
            break;
        case EditorAction::SelectInnerWord:
            if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
                std::optional<Range> range = core.inner_word_range();
                if (range && core.extend_selection_to_range(*range)) {
                    state.mode = Mode::Visual;
                    set_status(state, "VISUAL");
                } else {
                    set_status(state, "No word");
                }
            }
            break;
        case EditorAction::SelectAroundWord:
            if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
                std::optional<Range> range = core.a_word_range();
                if (range && core.extend_selection_to_range(*range)) {
                    state.mode = Mode::Visual;
                    set_status(state, "VISUAL");
                } else {
                    set_status(state, "No word");
                }
            }
            break;
    }
}

void execute_dispatch(EditorState &state, const KeyDispatch &dispatch, wint_t key) {
    if (!dispatch.expansion.empty()) {
        std::size_t repeat = take_repeat_count(state);
        for (std::size_t iteration = 0; iteration < repeat; ++iteration) {
            execute_expansion(state, dispatch.expansion);
        }
        return;
    }
    if (dispatch.action) {
        std::size_t repeat = take_repeat_count(state);
        if (action_defers_completion(*dispatch.action)) {
            execute_action(state, *dispatch.action, key);
            return;
        }
        std::size_t iterations = action_accepts_repeat(*dispatch.action) ? repeat : 1;
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            execute_action(state, *dispatch.action, key);
        }
    }
}

void handle_group_close(EditorState &state) {
    if (state.group_depth == 0) {
        return;
    }
    if (state.group_depth > 1) {
        record_group_input(state, ")", ')', false);
        --state.group_depth;
        return;
    }

    std::vector<EditorState::RecordedInput> inputs = state.group_inputs;
    std::size_t repeat = state.group_repeat_count;
    state.group_inputs.clear();
    state.group_depth = 0;
    state.group_repeat_count = 1;
    replay_group_inputs(state, inputs, repeat);
    active_core(state).end_compound_edit();
    set_status(state, mode_name(state.mode));
}

void handle_group_open(EditorState &state) {
    if (state.group_depth == 0) {
        state.group_repeat_count = take_repeat_count(state);
        state.group_inputs.clear();
        active_core(state).begin_compound_edit();
    } else {
        record_group_input(state, "(", '(', false);
    }
    ++state.group_depth;
    set_status(state, "(");
}

void process_input_token(EditorState &state, const std::string &token, wint_t key, bool printable) {
    if (mode_supports_command_language(state) && state.pending_motion != PendingMotion::None && printable) {
        record_group_input(state, token, key, printable);
        execute_pending_motion(state, static_cast<char32_t>(key));
        return;
    }

    if (mode_supports_command_language(state) && state.pending_motion == PendingMotion::None && state.pending_tokens.empty()) {
        if (token == "(") {
            handle_group_open(state);
            return;
        }
        if (token == ")" && state.group_depth > 0) {
            handle_group_close(state);
            return;
        }
        if (token_starts_repeat(state, token)) {
            record_group_input(state, token, key, printable);
            state.repeat_digits += token;
            set_status(state, state.repeat_digits);
            return;
        }
    }

    record_group_input(state, token, key, printable);

    KeyDispatch dispatch = dispatch_key_sequence(state.keybindings, mode_key(state), state.pending_tokens, token, printable);
    if (dispatch.waiting_for_more) {
        set_status(state, u32_to_utf8(utf8_to_u32(token)));
        return;
    }
    execute_dispatch(state, dispatch, key);
}

void handle_keymap_input(EditorState &state, wint_t key, bool is_special) {
    std::optional<std::string> token = key_token(key, is_special);
    if (!token) {
        return;
    }
    process_input_token(state, *token, key, is_printable_input(key, is_special));
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
    state.pending_motion_repeat_count = 1;
    state.repeat_digits.clear();
    active_core(state).set_cursor(*clicked);
    if (state.mode == Mode::Command) {
        set_status(state, ":");
        return;
    }
    if (state.mode == Mode::Search) {
        set_search_status(state, active_buffer_ui(state).search_pattern_valid ? "" : "invalid regex");
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
            state.pending_motion_repeat_count = 1;
            state.repeat_digits.clear();
            set_status(state, mode_name(state.mode));
            return;
        }
        if (!is_special) {
            std::optional<std::string> token = key_token(key, false);
            if (token) {
                record_group_input(state, *token, key, true);
            }
            execute_pending_motion(state, static_cast<char32_t>(key));
        }
        return;
    }

    handle_keymap_input(state, key, is_special);
}

void update_input_timeout(const EditorState &state) {
    std::optional<int> timeout_ms = state.runtime.idle_wait_timeout_ms();
    timeout(timeout_ms.has_value() ? *timeout_ms : -1);
}

void handle_service_events(EditorState &state) {
    for (const ServiceEvent &event : state.runtime.take_service_events()) {
        if (!event.command) {
            continue;
        }
        EditorCommand command = *event.command;
        if (!command.document_uri && event.document_uri) {
            command.document_uri = event.document_uri;
        }
        if (command.type == EditorCommandType::OpenLocation) {
            if (!command.document_uri || !command.position) {
                continue;
            }

            EditorState::JumpLocation origin = current_jump_location(state);
            EditorState::JumpLocation target = {*command.document_uri, file_path_from_uri(*command.document_uri), *command.position};
            if (same_jump_location(origin, target)) {
                set_status(state, "Already at definition");
                continue;
            }

            EditorBuffer *buffer = state.session.find_buffer_by_uri(*command.document_uri);
            if (!buffer) {
                std::string path = file_path_from_uri(*command.document_uri);
                if (path.empty()) {
                    set_status(state, "Definition target unavailable");
                    continue;
                }
                buffer = state.session.open_file(path, true);
                if (!buffer) {
                    set_status(state, "Could not open definition target");
                    continue;
                }
                state.buffer_ui.try_emplace(buffer->id);
            } else {
                state.session.switch_to_id(buffer->id);
            }

            buffer->core.set_cursor(*command.position);
            if (state.jump_back_stack.empty() || !same_jump_location(state.jump_back_stack.back(), origin)) {
                state.jump_back_stack.push_back(std::move(origin));
            }
            state.jump_forward_stack.clear();
            set_status(state, "Opened definition");
            continue;
        }
        EditorCore *target = &active_core(state);
        if (command.document_uri) {
            EditorBuffer *buffer = state.session.find_buffer_by_uri(*command.document_uri);
            if (!buffer) {
                continue;
            }
            if (command.type == EditorCommandType::MoveCursor) {
                state.session.switch_to_id(buffer->id);
            }
            target = &buffer->core;
        }
        EditorCommandResult result = apply_editor_command(*target, command);
        if (result.status_message) {
            set_status(state, *result.status_message);
        }
        (void)result;
    }
}

void run_editor(EditorState &state) {
    while (!state.should_quit) {
        for (EditorBuffer &buffer : state.session.buffers()) {
            state.runtime.dispatch_editor_events(buffer.core);
        }
        state.runtime.poll_services();
        normalize_selected_diagnostic(state);
        handle_service_events(state);
        refresh_syntax_highlights(state);
        int screen_rows = 0;
        int screen_cols = 0;
        getmaxyx(stdscr, screen_rows, screen_cols);
        int buffer_rows = screen_rows - 2;
        int buffer_cols = screen_cols - line_number_width(state) - 1;
        ensure_cursor_visible(state, buffer_rows, buffer_cols);
        draw_editor(state);
        update_input_timeout(state);
        handle_input(state);
    }
}

void open_startup_files(EditorState &state, int argc, char **argv) {
    if (argc <= 1) {
        return;
    }

    std::size_t first_buffer_id = state.session.active_buffer_id();
    bool opened_any = false;
    std::string last_failure;
    for (int index = 1; index < argc; ++index) {
        std::string path = argv[index];
        if (!opened_any) {
            if (active_core(state).load_file(path)) {
                opened_any = true;
                set_status(state, "Opened " + path);
            } else {
                last_failure = "Could not open file: " + path;
            }
            continue;
        }

        EditorBuffer *buffer = state.session.open_file(path, false);
        if (buffer) {
            state.buffer_ui.try_emplace(buffer->id);
            set_status(state, "Opened " + path);
        } else {
            last_failure = "Could not open file: " + path;
        }
    }

    state.session.switch_to_id(first_buffer_id);
    active_buffer_ui(state);
    if (!last_failure.empty()) {
        set_status(state, last_failure);
    }
}

int main(int argc, char **argv) {
    initialize_locale();
    EditorState state;
    try {
        state.config = load_editor_config();
        configure_logger(state.config.log_path);
        log_debug("editor startup");
        state.session.configure_clipboard(state.config.clipboard);
        state.keybindings = load_keybindings(state.config);
    } catch (const std::exception &error) {
        state.keybindings = load_embedded_keybindings();
        set_status(state, std::string("Keybindings config error: ") + error.what());
    }
    try {
        state.theme = load_theme(state.config);
    } catch (const std::exception &error) {
        state.theme = load_embedded_theme();
        set_status(state, std::string("Theme config error: ") + error.what());
    }
    if (!suspend_supported()) {
        remove_action_bindings(state.keybindings, EditorAction::Suspend);
    }
    active_buffer_ui(state);
    if (argc > 1) {
        open_startup_files(state, argc, argv);
    } else if (!state.config.source_path.empty()) {
        set_status(state, "Config: " + state.config.source_path);
    } else if (!state.keybindings.source_path.empty()) {
        set_status(state, "Keybindings: " + state.keybindings.source_path);
    }

    try {
        setup_terminal(state.theme);
        if (argc <= 1) {
            open_startup_file_picker(state);
        }
        if (!state.config.lsp_servers.empty()) {
            for (const LspServerConfig &server : state.config.lsp_servers) {
                state.runtime.add_service(std::make_unique<LspService>(server));
            }
        }
        state.runtime.start_services();
        run_editor(state);
        state.runtime.stop_services();
        teardown_terminal();
        return 0;
    } catch (const std::exception &error) {
        state.runtime.stop_services();
        teardown_terminal();
        std::fprintf(stderr, "medit: %s\n", error.what());
        return 1;
    }
}
