#include "editor_internal.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "lsp_service.hpp"
#include "process_utils.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

EditorWindow &active_window(EditorState &state) {
    return *state.windows.active_window();
}

const EditorWindow &active_window(const EditorState &state) {
    return *state.windows.active_window();
}

EditorBuffer &window_buffer(EditorState &state, std::size_t window_id) {
    return *state.session.find_buffer_by_id(state.windows.find_window(window_id)->buffer_id);
}

const EditorBuffer &window_buffer(const EditorState &state, std::size_t window_id) {
    return *state.session.find_buffer_by_id(state.windows.find_window(window_id)->buffer_id);
}

EditorBuffer &active_buffer(EditorState &state) {
    return window_buffer(state, state.windows.active_window_id());
}

const EditorBuffer &active_buffer(const EditorState &state) {
    return window_buffer(state, state.windows.active_window_id());
}

std::string buffer_text_utf8(const EditorBuffer &buffer) {
    std::string text;
    const auto &lines = buffer.core.lines();
    for (std::size_t row = 0; row < lines.size(); ++row) {
        if (row > 0) {
            text.push_back('\n');
        }
        text += u32_to_utf8(lines[row]);
    }
    return text;
}

JsonValue json_position(Position position) {
    return JsonValue{{"row", position.row}, {"column", position.column}};
}

JsonValue json_range(const Range &range) {
    return JsonValue{{"start", json_position(range.start)}, {"end", json_position(range.end)}};
}

const char *selection_mode_name(SelectionMode mode) {
    switch (mode) {
        case SelectionMode::Character:
            return "character";
        case SelectionMode::Line:
            return "line";
    }
    return "character";
}

JsonValue json_buffer_summary(const EditorState &state, const EditorBuffer &buffer) {
    bool active = buffer.id == active_buffer(state).id;
    JsonValue result = {
        {"id", buffer.id},
        {"document_uri", buffer.core.document_uri()},
        {"file_path", buffer.core.file_path() ? JsonValue(*buffer.core.file_path()) : JsonValue(nullptr)},
        {"display_name", buffer.core.display_file_name()},
        {"dirty", buffer.core.is_dirty()},
        {"line_count", buffer.core.line_count()},
        {"document_version", buffer.core.document_version()},
        {"active", active},
    };
    if (active) {
        result["cursor"] = json_position(displayed_cursor(state, state.windows.active_window_id()));
        if (std::optional<Range> selection = displayed_selection_range(state, state.windows.active_window_id())) {
            result["selection"] = json_range(*selection);
            result["selection_mode"] = selection_mode_name(buffer.core.selection_mode());
        } else {
            result["selection"] = nullptr;
            result["selection_mode"] = nullptr;
        }
    } else {
        result["cursor"] = nullptr;
        result["selection"] = nullptr;
        result["selection_mode"] = nullptr;
    }
    return result;
}

EditorCore &active_core(EditorState &state) {
    return active_buffer(state).core;
}

const EditorCore &active_core(const EditorState &state) {
    return active_buffer(state).core;
}

EditorCore &window_core(EditorState &state, std::size_t window_id) {
    return window_buffer(state, window_id).core;
}

const EditorCore &window_core(const EditorState &state, std::size_t window_id) {
    return window_buffer(state, window_id).core;
}

EditorState::WindowUiState &window_ui(EditorState &state, std::size_t window_id) {
    return state.window_ui.try_emplace(window_id).first->second;
}

const EditorState::WindowUiState &window_ui(const EditorState &state, std::size_t window_id) {
    auto found = state.window_ui.find(window_id);
    if (found != state.window_ui.end()) {
        return found->second;
    }
    return const_cast<EditorState &>(state).window_ui.try_emplace(window_id).first->second;
}

void clear_ast_selection_state(EditorState::WindowUiState &ui) {
    ui.ast_selection_document_uri.clear();
    ui.ast_selection_document_version = 0;
    ui.ast_selection_cursor = {};
    ui.ast_selection_ranges.clear();
    ui.ast_selection_index = 0;
}

EditorState::SyntaxUiState &buffer_syntax_ui(EditorState &state, std::size_t buffer_id) {
    return state.syntax_ui.try_emplace(buffer_id).first->second;
}

const EditorState::SyntaxUiState &buffer_syntax_ui(const EditorState &state, std::size_t buffer_id) {
    auto found = state.syntax_ui.find(buffer_id);
    if (found != state.syntax_ui.end()) {
        return found->second;
    }
    return const_cast<EditorState &>(state).syntax_ui.try_emplace(buffer_id).first->second;
}

EditorState::BufferUiState &buffer_ui_state(EditorState &state, std::size_t buffer_id) {
    return state.buffer_ui.try_emplace(buffer_id).first->second;
}

const EditorState::BufferUiState &buffer_ui_state(const EditorState &state, std::size_t buffer_id) {
    auto found = state.buffer_ui.find(buffer_id);
    if (found != state.buffer_ui.end()) {
        return found->second;
    }
    return const_cast<EditorState &>(state).buffer_ui.try_emplace(buffer_id).first->second;
}

EditorState::BufferUiState &active_buffer_cache(EditorState &state) {
    return buffer_ui_state(state, active_buffer(state).id);
}

const EditorState::BufferUiState &active_buffer_cache(const EditorState &state) {
    return buffer_ui_state(state, active_buffer(state).id);
}

EditorState::WindowUiState &active_buffer_ui(EditorState &state) {
    return window_ui(state, state.windows.active_window_id());
}

const EditorState::WindowUiState &active_buffer_ui(const EditorState &state) {
    return window_ui(state, state.windows.active_window_id());
}

Position displayed_cursor(const EditorState &state, std::size_t window_id) {
    return window_ui(state, window_id).view_state.cursor;
}

std::optional<Range> displayed_selection_range(const EditorState &state, std::size_t window_id) {
    const EditorState::WindowUiState &ui = window_ui(state, window_id);
    const EditorCore &core = window_core(state, window_id);
    if (!ui.view_state.selection_anchor) {
        return std::nullopt;
    }

    Position anchor = *ui.view_state.selection_anchor;
    Position cursor = ui.view_state.cursor;
    if (ui.view_state.selection_mode == SelectionMode::Line) {
        std::size_t start_row = anchor.row < cursor.row ? anchor.row : cursor.row;
        std::size_t end_row = anchor.row > cursor.row ? anchor.row : cursor.row;
        Position start{start_row, 0};
        Position end;
        if (end_row + 1 < core.line_count()) {
            end = {end_row + 1, 0};
        } else {
            end = {end_row, core.line_length(end_row)};
        }
        return Range{start, end};
    }

    Position cursor_extent = cursor.column < core.line_length(cursor.row) ? Position{cursor.row, cursor.column + 1} : cursor;
    Position anchor_extent = anchor.column < core.line_length(anchor.row) ? Position{anchor.row, anchor.column + 1} : anchor;
    if (position_less_than(cursor, anchor)) {
        return Range{cursor, anchor_extent};
    }
    return Range{anchor, cursor_extent};
}

void sync_window_view_from_core(EditorState &state, std::size_t window_id) {
    EditorState::WindowUiState &ui = window_ui(state, window_id);
    EditorViewState previous = ui.view_state;
    ui.view_state = window_core(state, window_id).view_state();
    auto same_optional_position = [](const std::optional<Position> &left, const std::optional<Position> &right) {
        if (!left.has_value() || !right.has_value()) {
            return !left.has_value() && !right.has_value();
        }
        return positions_equal(*left, *right);
    };
    if (!positions_equal(previous.cursor, ui.view_state.cursor) ||
        !same_optional_position(previous.selection_anchor, ui.view_state.selection_anchor) ||
        previous.selection_mode != ui.view_state.selection_mode) {
        clear_ast_selection_state(ui);
    }
}

void sync_core_view_from_window(EditorState &state, std::size_t window_id) {
    EditorState::WindowUiState &ui = window_ui(state, window_id);
    EditorCore &core = window_core(state, window_id);
    core.restore_view_state(ui.view_state, false);
    ui.view_state = core.view_state();
}

EditorState::PromptHistory &active_prompt_history(EditorState &state) {
    if (state.mode == Mode::Search) {
        return state.search_history;
    }
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
    std::u32string &buffer = state.mode == Mode::Search ? state.search_buffer : state.command_buffer;
    if (!history.browse_index) {
        if (!previous) {
            return;
        }
        history.draft = buffer;
        history.browse_index = history.entries.size() - 1;
    } else if (previous) {
        if (*history.browse_index == 0) {
            return;
        }
        --*history.browse_index;
    } else {
        if (*history.browse_index + 1 >= history.entries.size()) {
            buffer = history.draft;
            reset_prompt_history_navigation(history);
            return;
        }
        ++*history.browse_index;
    }
    buffer = history.entries[*history.browse_index];
}

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
    DiagnosticMessageError = 23,
    DiagnosticMessageWarning = 24,
    DiagnosticSelected = 25,
};

int codepoint_width(char32_t codepoint) {
    wchar_t wide = static_cast<wchar_t>(codepoint);
    int width = wcwidth(wide);
    return width > 0 ? width : 1;
}

std::size_t tab_display_width(std::size_t visual_column, std::size_t tabstop) {
    std::size_t width = tabstop == 0 ? 1 : tabstop;
    std::size_t remainder = visual_column % width;
    return remainder == 0 ? width : width - remainder;
}

std::size_t codepoint_display_width(char32_t codepoint, std::size_t visual_column, std::size_t tabstop) {
    if (codepoint == U'\t') {
        return tab_display_width(visual_column, tabstop);
    }
    return static_cast<std::size_t>(codepoint_width(codepoint));
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

std::string prefixed_message(const char *prefix, const std::string &value) {
    return std::string(prefix) + value;
}

std::string count_label(std::size_t count, const char *singular) {
    return std::to_string(count) + " " + singular + (count == 1 ? "" : "s");
}

std::string make_status_bar_left_text(
    const EditorState &state,
    const EditorCore &core,
    const std::string &language,
    const std::string &workspace) {
    return mode_name(state.mode) +
           "  " + core.display_file_name() +
           "  [b " + std::to_string(state.session.index_for_buffer_id(active_window(state).buffer_id).value_or(0) + 1) +
           "/" + std::to_string(state.session.buffer_count()) +
           "] [w " + std::to_string(state.windows.active_window_index() + 1) +
           "/" + std::to_string(state.windows.window_count()) +
           "]  " + language +
           "  ws:" + workspace;
}

std::string make_status_bar_right_text(const EditorCore &core, Position cursor) {
    return std::string(core.is_dirty() ? " [+]" : "") +
           "  " + std::to_string(cursor.row + 1) +
           ":" + std::to_string(cursor.column + 1) +
           "  rev " + std::to_string(core.current_revision());
}

void set_status(EditorState &state, const std::string &message) {
    state.status_message = message;
}

bool reload_editor_configuration(EditorState &state, std::string &error_message);
void handle_service_events(EditorState &state);
void begin_insert_session(EditorState &state);
void rebuild_popup_filter(EditorState &state);

void show_popup(EditorState &state, std::string title, std::u32string text) {
    state.popup.visible = true;
    state.popup.kind = PopupKind::Text;
    state.popup.title = std::move(title);
    state.popup.text = std::move(text);
    state.popup.items.clear();
    state.popup.filter.clear();
    state.popup.filtered_indices.clear();
    state.popup.selected_index = 0;
    state.popup.scroll_offset = 0;
    state.popup.originating_mode = state.mode;
}

void show_menu_popup(
    EditorState &state,
    std::string title,
    std::vector<PopupMenuItem> items,
    EditorState::PopupApplyTarget apply_target,
    std::u32string initial_filter,
    EditorState::PopupFilterMode filter_mode) {
    state.popup.visible = true;
    state.popup.kind = PopupKind::Menu;
    state.popup.title = std::move(title);
    state.popup.text.clear();
    state.popup.items = std::move(items);
    state.popup.filter = std::move(initial_filter);
    state.popup.filtered_indices.clear();
    state.popup.selected_index = 0;
    state.popup.scroll_offset = 0;
    state.popup.originating_mode = state.mode;
    state.popup.apply_target = apply_target;
    state.popup.filter_mode = filter_mode;
    rebuild_popup_filter(state);
}

void dismiss_popup(EditorState &state) {
    state.popup.visible = false;
    state.popup.kind = PopupKind::Text;
    state.popup.title.clear();
    state.popup.text.clear();
    state.popup.items.clear();
    state.popup.filter.clear();
    state.popup.filtered_indices.clear();
    state.popup.selected_index = 0;
    state.popup.scroll_offset = 0;
    state.popup.apply_target = EditorState::PopupApplyTarget::BufferText;
    state.popup.filter_mode = EditorState::PopupFilterMode::ContainsLabelOrDetail;
}

bool popup_accepts_input(const EditorState &state) {
    return state.popup.visible && state.popup.kind == PopupKind::Menu;
}

std::size_t popup_menu_visible_rows_for_screen(int screen_rows) {
    int available_rows = screen_rows - 2;
    return static_cast<std::size_t>(std::max(1, available_rows - 5));
}

void rebuild_popup_filter(EditorState &state) {
    state.popup.filtered_indices.clear();
    if (state.popup.kind != PopupKind::Menu) {
        return;
    }
    std::string filter = u32_to_utf8(state.popup.filter);
    std::string lowered_filter = ascii_lowercase(filter);
    for (std::size_t index = 0; index < state.popup.items.size(); ++index) {
        const PopupMenuItem &item = state.popup.items[index];
        bool matches = lowered_filter.empty();
        if (!matches) {
            if (state.popup.filter_mode == EditorState::PopupFilterMode::PrefixLabelOnly) {
                matches = ascii_lowercase(item.label).starts_with(lowered_filter);
            } else {
                std::string haystack = item.label;
                if (!item.detail.empty()) {
                    haystack += " " + item.detail;
                }
                matches = ascii_lowercase(haystack).contains(lowered_filter);
            }
        }
        if (matches) {
            state.popup.filtered_indices.push_back(index);
        }
    }
    if (state.popup.filtered_indices.empty()) {
        state.popup.selected_index = 0;
        state.popup.scroll_offset = 0;
        return;
    }
    if (state.popup.selected_index >= state.popup.filtered_indices.size()) {
        state.popup.selected_index = 0;
    }
    state.popup.scroll_offset = 0;
}

void ensure_popup_selection_visible(EditorState &state, std::size_t visible_rows) {
    if (!popup_accepts_input(state) || state.popup.filtered_indices.empty()) {
        return;
    }
    std::size_t clamped_visible_rows = std::max<std::size_t>(visible_rows, 1);
    if (state.popup.selected_index < state.popup.scroll_offset) {
        state.popup.scroll_offset = state.popup.selected_index;
    } else if (state.popup.selected_index >= state.popup.scroll_offset + clamped_visible_rows) {
        state.popup.scroll_offset = state.popup.selected_index - clamped_visible_rows + 1;
    }
    std::size_t max_offset = state.popup.filtered_indices.size() > clamped_visible_rows
        ? state.popup.filtered_indices.size() - clamped_visible_rows
        : 0;
    state.popup.scroll_offset = std::min(state.popup.scroll_offset, max_offset);
}

void move_popup_selection(EditorState &state, bool forward) {
    if (!popup_accepts_input(state) || state.popup.filtered_indices.empty()) {
        return;
    }
    if (forward) {
        state.popup.selected_index = (state.popup.selected_index + 1) % state.popup.filtered_indices.size();
    } else if (state.popup.selected_index == 0) {
        state.popup.selected_index = state.popup.filtered_indices.size() - 1;
    } else {
        --state.popup.selected_index;
    }
}

void apply_popup_selection(EditorState &state) {
    if (!popup_accepts_input(state) || state.popup.filtered_indices.empty()) {
        return;
    }
    PopupMenuItem item = state.popup.items[state.popup.filtered_indices[state.popup.selected_index]];
    Mode originating_mode = state.popup.originating_mode;
    EditorState::PopupApplyTarget apply_target = state.popup.apply_target;
    dismiss_popup(state);
    if (apply_target == EditorState::PopupApplyTarget::CommandBuffer) {
        state.command_buffer = utf8_to_u32(item.insert_text);
        set_status(state, ":" + item.insert_text);
        return;
    }

    EditorCore &core = active_core(state);
    std::u32string text = utf8_to_u32(item.insert_text);
    Position start = item.replace_range ? item.replace_range->start : core.cursor();
    bool applied = false;
    if (item.replace_range) {
        applied = core.replace_range(*item.replace_range, text);
    } else {
        applied = core.insert_text(core.cursor(), text);
    }
    if (applied) {
        core.set_cursor(EditorCommandAccess::position_after_text(core, start, text));
        sync_window_view_from_core(state, state.windows.active_window_id());
        set_status(state, "Completion applied");
    } else {
        set_status(state, "Completion failed");
    }
    if (originating_mode == Mode::Insert) {
        begin_insert_session(state);
    }
}

bool handle_popup_input(EditorState &state, const std::string &token) {
    if (!popup_accepts_input(state)) {
        return false;
    }
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);
    std::size_t visible_rows = popup_menu_visible_rows_for_screen(screen_rows);
    if (token == "up") {
        move_popup_selection(state, false);
        ensure_popup_selection_visible(state, visible_rows);
        return true;
    }
    if (token == "down") {
        move_popup_selection(state, true);
        ensure_popup_selection_visible(state, visible_rows);
        return true;
    }
    if (token == "tab") {
        move_popup_selection(state, true);
        ensure_popup_selection_visible(state, visible_rows);
        return true;
    }
    if (token == "shift-tab") {
        move_popup_selection(state, false);
        ensure_popup_selection_visible(state, visible_rows);
        return true;
    }
    if (token == "backspace") {
        if (!state.popup.filter.empty()) {
            state.popup.filter.pop_back();
            rebuild_popup_filter(state);
            ensure_popup_selection_visible(state, visible_rows);
        }
        return true;
    }
    if (token == "enter") {
        apply_popup_selection(state);
        return true;
    }
    if (token == "esc") {
        Mode originating_mode = state.popup.originating_mode;
        dismiss_popup(state);
        if (originating_mode == Mode::Insert) {
            begin_insert_session(state);
        }
        set_status(state, mode_name(state.mode));
        return true;
    }
    if (token == "printable") {
        return true;
    }
    if (!token.empty() && utf8_to_u32(token).size() == 1) {
        state.popup.filter += utf8_to_u32(token);
        rebuild_popup_filter(state);
        ensure_popup_selection_visible(state, visible_rows);
        return true;
    }
    dismiss_popup(state);
    return false;
}

void sync_active_window_buffer(EditorState &state) {
    if (const EditorWindow *window = state.windows.active_window()) {
        state.session.switch_to_id(window->buffer_id);
    }
}

void initialize_windows(EditorState &state) {
    state.windows = WindowManager(state.session.active_buffer_id());
    state.window_ui.clear();
    active_buffer_ui(state);
    sync_window_view_from_core(state, state.windows.active_window_id());
}

void show_buffer_in_active_window(EditorState &state, std::size_t buffer_id, bool reset_view) {
    state.windows.set_active_buffer_id(buffer_id);
    sync_active_window_buffer(state);
    if (reset_view) {
        active_buffer_ui(state) = EditorState::WindowUiState{};
        sync_window_view_from_core(state, state.windows.active_window_id());
    } else {
        sync_core_view_from_window(state, state.windows.active_window_id());
    }
}

void focus_window(EditorState &state, std::size_t window_id) {
    if (!state.windows.set_active_window(window_id)) {
        return;
    }
    sync_active_window_buffer(state);
    active_buffer_ui(state);
    sync_core_view_from_window(state, window_id);
}

void invalidate_syntax_cache(EditorState &state) {
    for (auto &[buffer_id, ui_state] : state.syntax_ui) {
        (void)buffer_id;
        ui_state.syntax_revision = std::numeric_limits<std::size_t>::max();
        ui_state.pending_syntax_revision = std::numeric_limits<std::size_t>::max();
        ui_state.syntax_dirty_since = {};
        ui_state.syntax_selection = {};
        ui_state.syntax_file_path.reset();
        ui_state.syntax_highlights.clear();
        ui_state.syntax_config_error_reported = false;
    }
}

void initialize_locale() {
    setlocale(LC_ALL, "");
}

bool suspend_supported() {
#if defined(SIGTSTP)
    return true;
#else
    return false;
#endif
}

void begin_insert_session(EditorState &state) {
    if (state.insert_session_active) {
        return;
    }
    active_core(state).begin_compound_edit();
    state.insert_session_active = true;
}

void end_insert_session(EditorState &state) {
    if (!state.insert_session_active) {
        return;
    }
    active_core(state).end_compound_edit();
    state.insert_session_active = false;
}

void enter_normal_mode(EditorState &state) {
    EditorCore &core = active_core(state);
    if (state.mode == Mode::Insert) {
        end_insert_session(state);
    }
    if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
        core.clear_selection();
    }
    state.mode = Mode::Normal;
    state.command_buffer.clear();
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.pending_replace_count = 0;
    state.repeat_digits.clear();
    state.search_buffer.clear();
    Position cursor = displayed_cursor(state, state.windows.active_window_id());
    std::size_t length = core.line_length(cursor.row);
    if (cursor.column > 0 && cursor.column == length) {
        core.set_cursor({cursor.row, cursor.column - 1});
    }
    set_status(state, mode_name(state.mode));
}

void enter_insert_mode(EditorState &state) {
    active_core(state).clear_selection();
    begin_insert_session(state);
    state.mode = Mode::Insert;
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.pending_replace_count = 0;
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
    state.pending_replace_count = 0;
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
    state.pending_replace_count = 0;
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
    state.pending_replace_count = 0;
    state.repeat_digits.clear();
    state.search_buffer.clear();
    set_status(state, "S");
}

void enter_search_mode(EditorState &state) {
    EditorCore &core = active_core(state);
    EditorState::WindowUiState &window_state = active_buffer_ui(state);
    core.clear_selection();
    state.mode = Mode::Search;
    state.search_buffer.clear();
    reset_prompt_history_navigation(state.search_history);
    window_state.search_origin = core.cursor();
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.pending_replace_count = 0;
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
            set_status(state, prefixed_message("Wrote ", core.display_file_name()));
        } else {
            set_status(state, core.file_path() ? "Write failed" : "No file name");
        }
        return;
    }
    if (core.save_current_file_as(argument)) {
        set_status(state, prefixed_message("Wrote ", argument));
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
        state.windows.set_active_buffer_id(buffer->id);
        sync_active_window_buffer(state);
        window_ui(state, state.windows.active_window_id()) = EditorState::WindowUiState{};
        log_debug("edit command opened path=" + argument);
        set_status(state, prefixed_message("Opened ", argument));
    } else {
        log_debug("edit command open failed path=" + argument);
        set_status(state, "Could not open file");
    }
}

const std::vector<DiagnosticEntryView> &sorted_diagnostics(const EditorState &state, std::size_t window_id) {
    const EditorBuffer &buffer = window_buffer(state, window_id);
    const EditorCore &core = buffer.core;
    EditorState::BufferUiState &buffer_ui = const_cast<EditorState &>(state).buffer_ui.try_emplace(buffer.id).first->second;
    if (buffer_ui.sorted_diagnostics_revision == core.diagnostics_revision()) {
        return buffer_ui.sorted_diagnostics;
    }

    buffer_ui.sorted_diagnostics.clear();
    const std::vector<Diagnostic> &source = core.diagnostics();
    buffer_ui.sorted_diagnostics.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        buffer_ui.sorted_diagnostics.push_back({index, source[index]});
    }
    std::sort(buffer_ui.sorted_diagnostics.begin(), buffer_ui.sorted_diagnostics.end(), [](const DiagnosticEntryView &left, const DiagnosticEntryView &right) {
        Range left_range = normalized_range(left.diagnostic.range);
        Range right_range = normalized_range(right.diagnostic.range);
        if (positions_equal(left_range.start, right_range.start)) {
            return position_less_than(left_range.end, right_range.end);
        }
        return position_less_than(left_range.start, right_range.start);
    });
    buffer_ui.sorted_diagnostics_revision = core.diagnostics_revision();
    return buffer_ui.sorted_diagnostics;
}

bool should_render_diagnostics(const EditorState &state, std::size_t window_id) {
    return state.diagnostics_visible && (
        state.mode != Mode::Insert ||
        effective_show_diagnostics_in_insert_mode(state.config, window_core(state, window_id).file_path()));
}

void normalize_selected_diagnostic(EditorState &state) {
    EditorState::WindowUiState &buffer_ui = active_buffer_ui(state);
    if (active_core(state).diagnostics().empty()) {
        buffer_ui.selected_diagnostic_index.reset();
        return;
    }
    if (!buffer_ui.selected_diagnostic_index || *buffer_ui.selected_diagnostic_index >= active_core(state).diagnostics().size()) {
        buffer_ui.selected_diagnostic_index = 0;
    }
}

void focus_diagnostic(EditorState &state, std::size_t diagnostic_index) {
    EditorState::WindowUiState &buffer_ui = active_buffer_ui(state);
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
    set_status(state, count_label(errors, "error") + ", " + count_label(warnings, "warning"));
}

void show_lsp_status(EditorState &state) {
    show_popup(state, "LSP Status", utf8_to_u32(state.runtime.status_summary()));
    set_status(state, "LSP status");
}

void show_tree_sitter_status(EditorState &state) {
    show_popup(
        state,
        "Tree-sitter Status",
        utf8_to_u32(tree_sitter_status_summary(state.config, active_core(state).file_path())));
    set_status(state, "Tree-sitter status");
}

void navigate_diagnostic(EditorState &state, bool forward) {
    EditorState::WindowUiState &buffer_ui = active_buffer_ui(state);
    const std::vector<DiagnosticEntryView> &diagnostics = sorted_diagnostics(state, state.windows.active_window_id());
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
    Position cursor = displayed_cursor(state, state.windows.active_window_id());
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

const std::vector<AnnotationEntryView> &sorted_annotations(const EditorState &state, std::size_t window_id) {
    const EditorBuffer &buffer = window_buffer(state, window_id);
    const EditorCore &core = buffer.core;
    bool show_diagnostics = should_render_diagnostics(state, window_id);
    EditorState::BufferUiState &buffer_ui = const_cast<EditorState &>(state).buffer_ui.try_emplace(buffer.id).first->second;
    std::size_t combined_revision = core.annotations_revision() ^ (core.diagnostics_revision() << 1);
    auto found_cache = buffer_ui.sorted_annotations.find(show_diagnostics);
    if (found_cache != buffer_ui.sorted_annotations.end() && found_cache->second.first == combined_revision) {
        return found_cache->second.second;
    }

    std::vector<AnnotationEntryView> annotations;
    std::vector<InlineAnnotation> projected = core.projected_annotations();
    annotations.reserve(projected.size());

    const std::vector<DiagnosticEntryView> &diagnostics = sorted_diagnostics(state, window_id);
    for (const InlineAnnotation &annotation : projected) {
        if (annotation.kind == AnnotationKind::Diagnostic && !show_diagnostics) {
            continue;
        }
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
    auto &cache_entry = buffer_ui.sorted_annotations[show_diagnostics];
    cache_entry.first = combined_revision;
    cache_entry.second = std::move(annotations);
    return cache_entry.second;
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

std::vector<VisualRow> build_visual_rows(const EditorState &state, std::size_t window_id, int buffer_cols) {
    std::vector<VisualRow> rows;
    std::vector<AnnotationEntryView> annotations = sorted_annotations(state, window_id);
    std::size_t annotation_index = 0;

    for (std::size_t row = 0; row < window_core(state, window_id).line_count(); ++row) {
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

const std::vector<VisualRow> &visual_rows_for_window(const EditorState &state, std::size_t window_id, int buffer_cols) {
    EditorBuffer const &buffer = window_buffer(state, window_id);
    EditorState::BufferUiState &buffer_state = const_cast<EditorState &>(state).buffer_ui.try_emplace(buffer.id).first->second;
    const EditorCore &core = window_core(state, window_id);
    bool show_diagnostics = should_render_diagnostics(state, window_id);
    VisualRowsCache &cache = buffer_state.visual_rows_caches[{buffer_cols, show_diagnostics}];
    bool live_text_may_change = state.insert_session_active;
    if (live_text_may_change || cache.buffer_cols != buffer_cols || cache.text_revision != core.current_revision() ||
        cache.diagnostics_revision != core.diagnostics_revision() ||
        cache.annotations_revision != core.annotations_revision() ||
        cache.show_diagnostics != show_diagnostics) {
        cache.buffer_cols = buffer_cols;
        cache.text_revision = core.current_revision();
        cache.diagnostics_revision = core.diagnostics_revision();
        cache.annotations_revision = core.annotations_revision();
        cache.show_diagnostics = show_diagnostics;
        cache.rows = build_visual_rows(state, window_id, buffer_cols);
    }
    return cache.rows;
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
        if (buffer.id == active_window(state).buffer_id) {
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
    if (state.session.find_buffer_by_id(buffer_id)) {
        show_buffer_in_active_window(state, buffer_id);
        set_status(state, prefixed_message("Switched to ", active_core(state).display_file_name()));
    } else {
        set_status(state, "No such buffer");
    }
}

void handle_buffer_delete_command(EditorState &state, bool force) {
    std::size_t closing_id = active_window(state).buffer_id;
    std::string closing_name = active_core(state).display_file_name();
    state.session.switch_to_id(closing_id);
    std::vector<EditorEvent> closed_events;
    if (!state.session.close_active_buffer(force, &closed_events)) {
        set_status(state, "Unsaved changes; use :bd! to close");
        return;
    }
    for (const EditorEvent &event : closed_events) {
        state.runtime.dispatch_editor_event(event);
    }
    state.buffer_ui.erase(closing_id);
    state.syntax_ui.erase(closing_id);
    std::size_t replacement_buffer_id = state.session.active_buffer_id();
    state.windows.replace_buffer_id(closing_id, replacement_buffer_id);
    for (EditorWindow const &window : state.windows.windows()) {
        if (window.buffer_id == replacement_buffer_id) {
            window_ui(state, window.id) = EditorState::WindowUiState{};
        }
    }
    sync_active_window_buffer(state);
    active_buffer_ui(state);
    set_status(state, prefixed_message("Closed ", closing_name));
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
    set_status(state, "Line " + std::to_string(target_row + 1));
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

std::string project_file_list_command(const std::optional<std::filesystem::path> &root = std::nullopt) {
    if (std::optional<std::string> fd = first_available_executable({"fd", "fdfind"})) {
        if (root && !root->empty()) {
            return *fd + " -t f -H -I -E .git . " + shell_single_quote(root->string());
        }
        return *fd + " -t f -H -I -E .git";
    }
    if (root && !root->empty()) {
        return "rg --files --hidden --no-ignore -g '!.git' " + shell_single_quote(root->string());
    }
    return "rg --files --hidden --no-ignore -g '!.git'";
}

std::string shell_printf_lines_command(const std::vector<std::string> &lines) {
    std::string command = "printf '%s\\n'";
    for (const std::string &line : lines) {
        command += " ";
        command += shell_single_quote(line);
    }
    return command;
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

std::filesystem::path theme_directory_for_config(const EditorConfig &config) {
    if (!config.source_path.empty()) {
        return std::filesystem::path(config.source_path).parent_path() / "medit" / "themes";
    }
    if (config.colors_path) {
        std::filesystem::path parent = config.colors_path->parent_path();
        if (parent.filename() == "themes") {
            return parent;
        }
        return parent / "themes";
    }
    return {};
}

bool update_meditrc_setting(
    const std::filesystem::path &meditrc_path,
    const std::string &key,
    const std::string &value,
    std::string &error_message) {
    std::ifstream input(meditrc_path);
    if (!input) {
        error_message = "could not open meditrc";
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    bool replaced = false;
    while (std::getline(input, line)) {
        std::string trimmed = trim_ascii_whitespace(line);
        std::size_t separator = trimmed.find('=');
        if (!trimmed.empty() && trimmed[0] != '#' && separator != std::string::npos &&
            trim_ascii_whitespace(trimmed.substr(0, separator)) == key) {
            lines.push_back(key + " = " + value);
            replaced = true;
        } else {
            lines.push_back(line);
        }
    }
    if (!replaced) {
        lines.push_back(key + " = " + value);
    }

    std::ofstream output(meditrc_path, std::ios::trunc);
    if (!output) {
        error_message = "could not write meditrc";
        return false;
    }
    for (std::size_t index = 0; index < lines.size(); ++index) {
        output << lines[index];
        if (index + 1 < lines.size()) {
            output << '\n';
        }
    }
    return true;
}

void handle_pick_theme_command(EditorState &state) {
    if (state.config.source_path.empty()) {
        set_status(state, "Theme picker needs a meditrc");
        return;
    }

    std::filesystem::path theme_dir = theme_directory_for_config(state.config);
    if (theme_dir.empty() || !std::filesystem::exists(theme_dir)) {
        set_status(state, "No theme directory");
        return;
    }

    std::vector<std::string> theme_names;
    for (const auto &entry : std::filesystem::directory_iterator(theme_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        theme_names.push_back("themes/" + entry.path().filename().string());
    }
    std::sort(theme_names.begin(), theme_names.end());
    if (theme_names.empty()) {
        set_status(state, "No themes found");
        return;
    }

    std::string error_message;
    std::optional<std::string> selection =
        run_picker_command(state, shell_printf_lines_command(theme_names) + " | fzf", error_message);
    if (!selection) {
        set_status(state, error_message);
        return;
    }

    std::string update_error;
    std::filesystem::path meditrc_path = state.config.source_path;
    if (!update_meditrc_setting(meditrc_path, "colors", *selection, update_error)) {
        set_status(state, update_error);
        return;
    }

    std::string reload_error;
    if (reload_editor_configuration(state, reload_error)) {
        set_status(state, "Theme: " + *selection);
    } else {
        set_status(state, "Config reload failed: " + reload_error);
    }
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
    std::optional<std::string> selection =
        run_picker_command(state, project_file_list_command() + " | fzf", error_message);
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

void open_startup_file_picker(EditorState &state, const std::optional<std::filesystem::path> &root = std::nullopt) {
    std::string error_message;
    std::optional<std::string> selection =
        run_picker_command(state, project_file_list_command(root) + " | fzf", error_message);
    if (!selection) {
        log_debug("startup picker canceled/error: " + error_message);
        if (!error_message.empty()) {
            set_status(state, error_message);
        }
        return;
    }

    std::filesystem::path resolved = *selection;
    if (resolved.is_relative()) {
        resolved = (root && !root->empty() ? *root : std::filesystem::current_path()) / resolved;
    }
    resolved = resolved.lexically_normal();
    log_debug("startup picker resolved path=" + resolved.string());

    EditorBuffer *buffer = state.session.open_file(resolved.string(), true);
    if (!buffer) {
        log_debug("startup picker open failed path=" + resolved.string());
        set_status(state, "Could not open file");
        return;
    }
    state.windows.set_active_buffer_id(buffer->id);
    sync_active_window_buffer(state);
    active_buffer_ui(state);
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
        if (std::optional<std::size_t> window_id = state.windows.find_window_showing_buffer(existing->id)) {
            focus_window(state, *window_id);
        } else {
            show_buffer_in_active_window(state, existing->id);
        }
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
    state.windows.set_active_buffer_id(buffer->id);
    sync_active_window_buffer(state);
    active_buffer_ui(state);
    active_core(state).set_cursor({row, column});
    set_status(state, "Opened " + path);
}

void request_definition(EditorState &state) {
    EditorCore &core = active_core(state);
    state.runtime.dispatch_editor_events(core);
    ServiceRequest request;
    request.type = ServiceRequestType::GoToDefinition;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position(core.cursor());
    state.runtime.dispatch_service_request(request);
    set_status(state, "Definition requested");
}

void request_hover(EditorState &state) {
    EditorCore &core = active_core(state);
    state.runtime.dispatch_editor_events(core);
    ServiceRequest request;
    request.type = ServiceRequestType::Hover;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position(core.cursor());
    request.document_version = core.document_version();
    state.runtime.dispatch_service_request(request);
    state.runtime.poll_services();
    handle_service_events(state);
    set_status(state, "Hover requested");
}

void request_completion(EditorState &state) {
    EditorCore &core = active_core(state);
    if (state.mode == Mode::Insert) {
        end_insert_session(state);
    }
    state.runtime.dispatch_editor_events(core);
    auto is_identifier_char = [](char32_t codepoint) {
        return (codepoint >= U'a' && codepoint <= U'z') || (codepoint >= U'A' && codepoint <= U'Z') ||
            (codepoint >= U'0' && codepoint <= U'9') || codepoint == U'_';
    };

    Position cursor = core.cursor();
    Position start = cursor;
    if (cursor.row < core.line_count()) {
        const std::u32string &line = core.lines()[cursor.row];
        std::size_t column = std::min(cursor.column, line.size());
        while (column > 0 && is_identifier_char(line[column - 1])) {
            --column;
        }
        start = {cursor.row, column};
    }

    ServiceRequest request;
    request.type = ServiceRequestType::Completion;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position(cursor);
    request.document_version = core.document_version();
    request.completion_range = Range{start, cursor};
    request.completion_prefix = u32_to_utf8(core.read_text(*request.completion_range));
    state.runtime.dispatch_service_request(request);
    state.runtime.poll_services();
    handle_service_events(state);
    set_status(state, "Completion requested");
}

void request_selection_range(EditorState &state) {
    EditorCore &core = active_core(state);
    state.runtime.dispatch_editor_events(core);
    ServiceRequest request;
    request.type = ServiceRequestType::SelectionRange;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position(core.cursor());
    request.document_version = core.document_version();
    state.runtime.dispatch_service_request(request);
    state.runtime.poll_services();
    handle_service_events(state);
}

void select_enclosing_ast(EditorState &state) {
    EditorState::WindowUiState &ui = active_buffer_ui(state);
    EditorCore &core = active_core(state);
    bool cache_valid = ui.ast_selection_document_uri == core.document_uri() &&
        ui.ast_selection_document_version == core.document_version() && !ui.ast_selection_ranges.empty();
    if (!cache_valid) {
        request_selection_range(state);
        return;
    }
    if (ui.ast_selection_index + 1 < ui.ast_selection_ranges.size()) {
        ++ui.ast_selection_index;
        std::string cached_uri = ui.ast_selection_document_uri;
        std::size_t cached_version = ui.ast_selection_document_version;
        Position cached_cursor = ui.ast_selection_cursor;
        std::vector<Range> cached_ranges = ui.ast_selection_ranges;
        std::size_t cached_index = ui.ast_selection_index;
        core.set_selection_range(ui.ast_selection_ranges[ui.ast_selection_index], SelectionMode::Character);
        state.mode = Mode::Visual;
        sync_window_view_from_core(state, state.windows.active_window_id());
        ui.ast_selection_document_uri = std::move(cached_uri);
        ui.ast_selection_document_version = cached_version;
        ui.ast_selection_cursor = cached_cursor;
        ui.ast_selection_ranges = std::move(cached_ranges);
        ui.ast_selection_index = cached_index;
        set_status(state, "Selected enclosing AST node");
    } else {
        set_status(state, "No enclosing AST range");
    }
}

void select_inner_ast(EditorState &state) {
    EditorState::WindowUiState &ui = active_buffer_ui(state);
    EditorCore &core = active_core(state);
    bool cache_valid = ui.ast_selection_document_uri == core.document_uri() &&
        ui.ast_selection_document_version == core.document_version() && !ui.ast_selection_ranges.empty();
    if (!cache_valid) {
        set_status(state, "No smaller AST range");
        return;
    }
    if (ui.ast_selection_index == 0) {
        set_status(state, "No smaller AST range");
        return;
    }
    --ui.ast_selection_index;
    std::string cached_uri = ui.ast_selection_document_uri;
    std::size_t cached_version = ui.ast_selection_document_version;
    Position cached_cursor = ui.ast_selection_cursor;
    std::vector<Range> cached_ranges = ui.ast_selection_ranges;
    std::size_t cached_index = ui.ast_selection_index;
    core.set_selection_range(ui.ast_selection_ranges[ui.ast_selection_index], SelectionMode::Character);
    state.mode = Mode::Visual;
    sync_window_view_from_core(state, state.windows.active_window_id());
    ui.ast_selection_document_uri = std::move(cached_uri);
    ui.ast_selection_document_version = cached_version;
    ui.ast_selection_cursor = cached_cursor;
    ui.ast_selection_ranges = std::move(cached_ranges);
    ui.ast_selection_index = cached_index;
    set_status(state, "Selected inner AST node");
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
    for (const std::string &line : lines) {
        std::filesystem::path path = std::filesystem::path(line).lexically_normal();
        if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
            matches.push_back(std::move(path));
        }
    }
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
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
    std::vector<std::filesystem::path> matches = search_workspace_for_file(workspace_root, *token);
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
    return {core.document_uri(), core.file_path(), displayed_cursor(state, state.windows.active_window_id())};
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
        show_buffer_in_active_window(state, buffer->id);
    } else {
        if (std::optional<std::size_t> window_id = state.windows.find_window_showing_buffer(buffer->id)) {
            focus_window(state, *window_id);
        } else {
            show_buffer_in_active_window(state, buffer->id);
        }
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
    std::optional<Range> selection = displayed_selection_range(state, state.windows.active_window_id());
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

struct SubstituteCommand {
    bool whole_buffer = false;
    std::string pattern;
    std::string replacement;
    bool global = false;
};

enum class NamedEditorCommand {
    Write,
    Quit,
    ForceQuit,
    WriteQuit,
    WriteIfChangedQuit,
    Edit,
    Buffers,
    Buffer,
    NextBuffer,
    PreviousBuffer,
    DeleteBuffer,
    ForceDeleteBuffer,
    FindFile,
    Grep,
    PickTheme,
    ReloadConfig,
    Diagnostics,
    LspStatus,
    TreeSitterStatus,
};

struct NamedEditorCommandInfo {
    std::string_view name;
    std::string_view detail;
    std::string_view completion_text;
    NamedEditorCommand command;
};

constexpr std::array<NamedEditorCommandInfo, 19> kNamedEditorCommands{{
    {"w", "write current buffer", "w", NamedEditorCommand::Write},
    {"q", "quit", "q", NamedEditorCommand::Quit},
    {"q!", "force quit", "q!", NamedEditorCommand::ForceQuit},
    {"wq", "write and quit", "wq", NamedEditorCommand::WriteQuit},
    {"x", "write and quit if modified", "x", NamedEditorCommand::WriteIfChangedQuit},
    {"e", "edit file in active window", "e ", NamedEditorCommand::Edit},
    {"buffers", "list open buffers", "buffers", NamedEditorCommand::Buffers},
    {"buffer", "switch to buffer", "buffer ", NamedEditorCommand::Buffer},
    {"bnext", "next buffer", "bnext", NamedEditorCommand::NextBuffer},
    {"bprev", "previous buffer", "bprev", NamedEditorCommand::PreviousBuffer},
    {"bd", "close current buffer", "bd", NamedEditorCommand::DeleteBuffer},
    {"bd!", "force close current buffer", "bd!", NamedEditorCommand::ForceDeleteBuffer},
    {"find-file", "pick file with fzf", "find-file", NamedEditorCommand::FindFile},
    {"grep", "grep with rg and fzf", "grep ", NamedEditorCommand::Grep},
    {"pick-theme", "pick a color theme", "pick-theme", NamedEditorCommand::PickTheme},
    {"reload-config", "reload medit configuration", "reload-config", NamedEditorCommand::ReloadConfig},
    {"diagnostics", "show diagnostics summary", "diagnostics", NamedEditorCommand::Diagnostics},
    {"lsp-status", "show language server status", "lsp-status", NamedEditorCommand::LspStatus},
    {"tree-sitter-status", "show tree-sitter status", "tree-sitter-status", NamedEditorCommand::TreeSitterStatus},
}};

bool parse_substitute_component(
    std::string_view command,
    std::size_t &index,
    char delimiter,
    std::string &component,
    std::string &error_message) {
    while (index < command.size()) {
        char ch = command[index];
        if (ch == delimiter) {
            ++index;
            return true;
        }
        if (ch == '\\' && index + 1 < command.size() &&
            (command[index + 1] == delimiter || command[index + 1] == '\\')) {
            component.push_back(command[index + 1]);
            index += 2;
            continue;
        }
        component.push_back(ch);
        ++index;
    }
    error_message = "unterminated substitute command";
    return false;
}

bool parse_substitute_command(std::string_view command, SubstituteCommand &parsed, std::string &error_message) {
    std::size_t index = 0;
    if (command.starts_with("%s")) {
        parsed.whole_buffer = true;
        index = 2;
    } else if (command.starts_with("s")) {
        index = 1;
    } else {
        return false;
    }

    if (index >= command.size()) {
        error_message = "missing substitute delimiter";
        return false;
    }
    char delimiter = command[index++];
    if (std::isalnum(static_cast<unsigned char>(delimiter)) || std::isspace(static_cast<unsigned char>(delimiter))) {
        error_message = "invalid substitute delimiter";
        return false;
    }

    if (!parse_substitute_component(command, index, delimiter, parsed.pattern, error_message)) {
        return false;
    }
    if (!parse_substitute_component(command, index, delimiter, parsed.replacement, error_message)) {
        return false;
    }

    for (; index < command.size(); ++index) {
        if (command[index] == 'g') {
            parsed.global = true;
            continue;
        }
        error_message = "unsupported substitute flags";
        return false;
    }
    return true;
}

void handle_substitute_command(EditorState &state, const SubstituteCommand &command) {
    EditorCore &core = active_core(state);
    std::size_t start_row = command.whole_buffer ? 0 : core.cursor().row;
    std::size_t end_row = command.whole_buffer ? core.line_count() - 1 : core.cursor().row;
    std::string error_message;
    std::size_t substitutions =
        core.substitute_regex(start_row, end_row, command.pattern, command.replacement, command.global, error_message);
    if (!error_message.empty()) {
        set_status(state, error_message);
        return;
    }
    if (substitutions == 0) {
        set_status(state, "Pattern not found");
        return;
    }
    set_status(state, count_label(substitutions, "substitution"));
}

std::vector<PopupMenuItem> command_completion_items() {
    std::vector<PopupMenuItem> items;
    items.reserve(kNamedEditorCommands.size() + 2);
    for (const NamedEditorCommandInfo &command : kNamedEditorCommands) {
        items.push_back(
            {std::string(command.name), std::string(command.detail), std::string(command.completion_text), std::nullopt});
    }
    items.push_back({"s", "substitute on current line", "s/", std::nullopt});
    items.push_back({"%s", "substitute in whole buffer", "%s/", std::nullopt});
    return items;
}

std::optional<NamedEditorCommand> named_editor_command_from_verb(std::string_view verb) {
    auto found = std::find_if(
        kNamedEditorCommands.begin(),
        kNamedEditorCommands.end(),
        [verb](const NamedEditorCommandInfo &command) { return command.name == verb; });
    if (found == kNamedEditorCommands.end()) {
        return std::nullopt;
    }
    return found->command;
}

void execute_named_editor_command(
    EditorState &state,
    NamedEditorCommand command,
    const std::string &argument) {
    switch (command) {
        case NamedEditorCommand::Write:
            handle_write_command(state, argument);
            break;
        case NamedEditorCommand::Quit:
            handle_quit_command(state, false);
            break;
        case NamedEditorCommand::ForceQuit:
            handle_quit_command(state, true);
            break;
        case NamedEditorCommand::WriteQuit:
            handle_write_quit_command(state, argument);
            break;
        case NamedEditorCommand::WriteIfChangedQuit:
            handle_write_quit_command(state, argument);
            break;
        case NamedEditorCommand::Edit:
            handle_edit_command(state, argument);
            break;
        case NamedEditorCommand::Buffers:
            set_status(state, buffers_summary(state));
            break;
        case NamedEditorCommand::Buffer:
            handle_buffer_switch_command(state, argument);
            break;
        case NamedEditorCommand::NextBuffer:
            state.session.next_buffer();
            set_status(state, prefixed_message("Switched to ", active_core(state).display_file_name()));
            break;
        case NamedEditorCommand::PreviousBuffer:
            state.session.previous_buffer();
            set_status(state, prefixed_message("Switched to ", active_core(state).display_file_name()));
            break;
        case NamedEditorCommand::DeleteBuffer:
            handle_buffer_delete_command(state, false);
            break;
        case NamedEditorCommand::ForceDeleteBuffer:
            handle_buffer_delete_command(state, true);
            break;
        case NamedEditorCommand::FindFile:
            handle_find_file_command(state);
            break;
        case NamedEditorCommand::Grep:
            handle_grep_command(state, argument);
            break;
        case NamedEditorCommand::PickTheme:
            handle_pick_theme_command(state);
            break;
        case NamedEditorCommand::ReloadConfig:
            {
            std::string error_message;
            if (reload_editor_configuration(state, error_message)) {
                set_status(state, "Reloaded config");
            } else {
                set_status(state, "Config reload failed: " + error_message);
            }
            break;
            }
        case NamedEditorCommand::Diagnostics:
            show_diagnostics_summary(state);
            break;
        case NamedEditorCommand::LspStatus:
            show_lsp_status(state);
            break;
        case NamedEditorCommand::TreeSitterStatus:
            show_tree_sitter_status(state);
            break;
    }
}

void show_command_completion(EditorState &state) {
    if (state.mode != Mode::Command || state.command_prompt_kind != CommandPromptKind::EditorCommand) {
        return;
    }
    std::string command = u32_to_utf8(state.command_buffer);
    if (command.contains(' ') || command.contains('\t')) {
        set_status(state, "Complete the command name only");
        return;
    }
    show_menu_popup(
        state,
        "Commands",
        command_completion_items(),
        EditorState::PopupApplyTarget::CommandBuffer,
        state.command_buffer,
        EditorState::PopupFilterMode::PrefixLabelOnly);
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
    SubstituteCommand substitute;
    std::string substitute_error;
    if (parse_substitute_command(command, substitute, substitute_error)) {
        handle_substitute_command(state, substitute);
    } else if (!substitute_error.empty()) {
        set_status(state, substitute_error);
    } else if (std::all_of(verb.begin(), verb.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        handle_goto_line_command(state, verb);
    } else if (std::optional<NamedEditorCommand> named = named_editor_command_from_verb(verb)) {
        execute_named_editor_command(state, *named, argument);
    } else {
        set_status(state, "Unknown command: " + verb);
    }

    if (!state.should_quit) {
        state.mode = Mode::Normal;
        state.command_buffer.clear();
        state.pending_tokens.clear();
    }
}

void set_search_status(EditorState &state, const std::string &suffix) {
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
    EditorState::BufferUiState &buffer_ui = active_buffer_cache(state);
    const EditorCore &core = active_core(state);
    buffer_ui.search_matches.clear();
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

void rebuild_search_matches(EditorState &state, std::size_t window_id) {
    if (window_id == state.windows.active_window_id()) {
        rebuild_search_matches(state);
        return;
    }

    EditorState::BufferUiState &buffer_ui = buffer_ui_state(state, window_buffer(state, window_id).id);
    const EditorCore &core = window_core(state, window_id);
    buffer_ui.search_matches.clear();
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
    EditorState::WindowUiState &window_state = active_buffer_ui(state);
    EditorState::BufferUiState &buffer_ui = active_buffer_cache(state);
    EditorCore &core = active_core(state);
    if (buffer_ui.search_matches_version != core.document_version() ||
        buffer_ui.compiled_search_pattern_utf8 != u32_to_utf8(buffer_ui.active_search_pattern)) {
        rebuild_search_matches(state);
    }

    if (!buffer_ui.search_pattern_valid || buffer_ui.search_matches.empty()) {
        if (move_to_best_match) {
            window_state.current_search_match_index.reset();
        }
        return;
    }

    Position anchor = move_to_best_match ? window_state.search_origin : displayed_cursor(state, state.windows.active_window_id());
    std::size_t best_index = 0;
    bool found_best = false;
    for (std::size_t index = 0; index < buffer_ui.search_matches.size(); ++index) {
        if (!position_less_than(buffer_ui.search_matches[index].start, anchor)) {
            best_index = index;
            found_best = true;
            break;
        }
    }
    window_state.current_search_match_index = found_best ? best_index : 0;
    if (move_to_best_match) {
        core.set_cursor(buffer_ui.search_matches[*window_state.current_search_match_index].start);
        sync_window_view_from_core(state, state.windows.active_window_id());
    }
}

void refresh_search_matches_for_window(EditorState &state, std::size_t window_id) {
    EditorState::BufferUiState &buffer_ui = buffer_ui_state(state, window_buffer(state, window_id).id);
    EditorCore &core = window_core(state, window_id);
    if (buffer_ui.search_matches_version != core.document_version() ||
        buffer_ui.compiled_search_pattern_utf8 != u32_to_utf8(buffer_ui.active_search_pattern)) {
        rebuild_search_matches(state, window_id);
    }
}

void navigate_search_match(EditorState &state, bool forward) {
    EditorState::WindowUiState &window_state = active_buffer_ui(state);
    EditorState::BufferUiState &buffer_ui = active_buffer_cache(state);
    EditorCore &core = active_core(state);
    if (buffer_ui.search_matches.empty()) {
        set_status(state, buffer_ui.search_pattern_valid ? "No search matches" : "invalid regex");
        return;
    }

    std::size_t index = window_state.current_search_match_index.value_or(0);
    if (forward) {
        index = (index + 1) % buffer_ui.search_matches.size();
    } else {
        index = index == 0 ? buffer_ui.search_matches.size() - 1 : index - 1;
    }
    window_state.current_search_match_index = index;
    core.set_cursor(buffer_ui.search_matches[index].start);
    sync_window_view_from_core(state, state.windows.active_window_id());
    set_status(state, forward ? "Next match" : "Previous match");
}

int main(int argc, char **argv) {
    initialize_locale();
    EditorState state;
    std::optional<std::string> startup_picker_root;
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
    initialize_windows(state);
    if (argc > 1) {
        startup_picker_root = open_startup_files(state, argc, argv);
    } else if (!state.config.source_path.empty()) {
        set_status(state, "Config: " + state.config.source_path);
    } else if (!state.keybindings.source_path.empty()) {
        set_status(state, "Keybindings: " + state.keybindings.source_path);
    }

    try {
        setup_terminal(state.theme);
        if (startup_picker_root) {
            open_startup_file_picker(state, std::filesystem::path(*startup_picker_root));
        } else if (argc <= 1) {
            open_startup_file_picker(state);
        }
        if (state.config.control_socket_path) {
            std::string control_error;
            if (state.control_server.start(*state.config.control_socket_path, control_error)) {
                log_debug("control socket started path=" + state.config.control_socket_path->string());
            } else {
                set_status(state, "Control socket failed: " + control_error);
                log_debug(
                    "control socket start failed path=" + state.config.control_socket_path->string() +
                    " error=" + control_error);
            }
        }
        if (!state.config.lsp_servers.empty()) {
            for (const LspServerConfig &server : state.config.lsp_servers) {
                state.runtime.add_service(std::make_unique<LspService>(server));
            }
        }
        render_frame(state);
        state.runtime.start_services();
        run_editor(state);
        state.runtime.stop_services();
        state.control_server.stop();
        teardown_terminal();
        return 0;
    } catch (const std::exception &error) {
        state.runtime.stop_services();
        state.control_server.stop();
        teardown_terminal();
        std::fprintf(stderr, "medit: %s\n", error.what());
        return 1;
    }
}
