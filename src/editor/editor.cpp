#include "editor_internal.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "lsp_service.hpp"
#include "process_utils.hpp"
#include "string_utils.hpp"

#ifdef _WIN32
#include "pdcurses_compat.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>

import theme;

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

// EditorState member functions
EditorWindow &EditorState::active_window() {
    return *windows.active_window();
}

const EditorWindow &EditorState::active_window() const {
    return *windows.active_window();
}

EditorBuffer &EditorState::window_buffer(std::size_t window_id) {
    return *session.find_buffer_by_id(windows.find_window(window_id)->buffer_id);
}

const EditorBuffer &EditorState::window_buffer(std::size_t window_id) const {
    return *session.find_buffer_by_id(windows.find_window(window_id)->buffer_id);
}

EditorBuffer &EditorState::active_buffer() {
    return window_buffer(windows.active_window_id());
}

const EditorBuffer &EditorState::active_buffer() const {
    return window_buffer(windows.active_window_id());
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
    bool active = buffer.id == state.active_buffer().id;
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
        result["cursor"] = json_position(state.displayed_cursor(state.windows.active_window_id()));
        if (std::optional<Range> selection = state.displayed_selection_range(state.windows.active_window_id())) {
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

EditorCore &EditorState::active_core() {
    return active_buffer().core;
}

const EditorCore &EditorState::active_core() const {
    return active_buffer().core;
}

EditorCore &EditorState::window_core(std::size_t window_id) {
    return window_buffer(window_id).core;
}

const EditorCore &EditorState::window_core(std::size_t window_id) const {
    return window_buffer(window_id).core;
}

EditorState::WindowUiState &EditorState::window_ui(std::size_t window_id) {
    return this->window_ui_map.try_emplace(window_id).first->second;
}

const EditorState::WindowUiState &EditorState::window_ui(std::size_t window_id) const {
    auto found = this->window_ui_map.find(window_id);
    if (found != this->window_ui_map.end()) {
        return found->second;
    }
    return const_cast<EditorState &>(*this).window_ui_map.try_emplace(window_id).first->second;
}

void clear_ast_selection_state(EditorState::WindowUiState &ui) {
    ui.ast_selection_document_uri.clear();
    ui.ast_selection_document_version = 0;
    ui.ast_selection_cursor = {};
    ui.ast_selection_ranges.clear();
    ui.ast_selection_index = 0;
}

EditorState::SyntaxUiState &EditorState::buffer_syntax_ui(std::size_t buffer_id) {
    return this->syntax_ui_map.try_emplace(buffer_id).first->second;
}

const EditorState::SyntaxUiState &EditorState::buffer_syntax_ui(std::size_t buffer_id) const {
    auto found = this->syntax_ui_map.find(buffer_id);
    if (found != this->syntax_ui_map.end()) {
        return found->second;
    }
    return const_cast<EditorState &>(*this).syntax_ui_map.try_emplace(buffer_id).first->second;
}

EditorState::BufferUiState &EditorState::buffer_ui_state(std::size_t buffer_id) {
    return this->buffer_ui_map.try_emplace(buffer_id).first->second;
}

const EditorState::BufferUiState &EditorState::buffer_ui_state(std::size_t buffer_id) const {
    auto found = this->buffer_ui_map.find(buffer_id);
    if (found != this->buffer_ui_map.end()) {
        return found->second;
    }
    return const_cast<EditorState &>(*this).buffer_ui_map.try_emplace(buffer_id).first->second;
}

EditorState::BufferUiState &EditorState::active_buffer_cache() {
    return buffer_ui_state(active_buffer().id);
}

const EditorState::BufferUiState &EditorState::active_buffer_cache() const {
    return buffer_ui_state(active_buffer().id);
}

EditorState::WindowUiState &EditorState::active_buffer_ui() {
    return window_ui(windows.active_window_id());
}

const EditorState::WindowUiState &EditorState::active_buffer_ui() const {
    return window_ui(windows.active_window_id());
}

Position EditorState::displayed_cursor(std::size_t window_id) const {
    return window_ui(window_id).view_state.cursor;
}

std::optional<Range> EditorState::displayed_selection_range(std::size_t window_id) const {
    const WindowUiState &ui = window_ui(window_id);
    const EditorCore &core = window_core(window_id);
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
    if ((cursor < anchor)) {
        return Range{cursor, anchor_extent};
    }
    return Range{anchor, cursor_extent};
}

void EditorState::capture_command_selection_snapshot() {
    std::optional<Range> selection = displayed_selection_range(windows.active_window_id());
    if (!selection) {
        command_selection_snapshot.reset();
        return;
    }
    command_selection_snapshot = CommandSelectionSnapshot{active_buffer().id, *selection};
}

void EditorState::clear_command_selection_snapshot() {
    command_selection_snapshot.reset();
}

std::optional<EditorState::CommandSelectionSnapshot> EditorState::selection_snapshot_for_commands() const {
    if (std::optional<Range> selection = displayed_selection_range(windows.active_window_id())) {
        return CommandSelectionSnapshot{active_buffer().id, *selection};
    }
    if (mode == Mode::Command && command_prompt_kind == CommandPromptKind::EditorCommand && command_selection_snapshot) {
        return command_selection_snapshot;
    }
    return std::nullopt;
}

std::optional<std::u32string> EditorState::selection_text_for_commands() const {
    std::optional<CommandSelectionSnapshot> snapshot = selection_snapshot_for_commands();
    if (!snapshot) {
        return std::nullopt;
    }
    const EditorBuffer *buffer = session.find_buffer_by_id(snapshot->buffer_id);
    if (buffer == nullptr) {
        return std::nullopt;
    }
    return buffer->core.read_text(snapshot->range);
}

bool EditorState::replace_selection_for_commands(const std::u32string &text) {
    std::optional<CommandSelectionSnapshot> snapshot = selection_snapshot_for_commands();
    if (!snapshot) {
        return false;
    }
    EditorBuffer *buffer = session.find_buffer_by_id(snapshot->buffer_id);
    if (buffer == nullptr) {
        return false;
    }
    bool changed = buffer->core.replace_range(snapshot->range, text);
    if (mode == Mode::Command && command_prompt_kind == CommandPromptKind::EditorCommand) {
        command_selection_snapshot.reset();
    }
    return changed;
}

void EditorState::sync_window_view_from_core(std::size_t window_id) {
EditorState::WindowUiState &ui = window_ui(window_id);
    EditorViewState previous = ui.view_state;
    ui.view_state = window_core(window_id).view_state();
    auto same_optional_position = [](const std::optional<Position> &left, const std::optional<Position> &right) {
        if (!left.has_value() || !right.has_value()) {
            return !left.has_value() && !right.has_value();
        }
        return (*left == *right);
    };
    if (!(previous.cursor == ui.view_state.cursor) ||
        !same_optional_position(previous.selection_anchor, ui.view_state.selection_anchor) ||
        previous.selection_mode != ui.view_state.selection_mode) {
        clear_ast_selection_state(ui);
    }
}

void EditorState::sync_core_view_from_window(std::size_t window_id) {
EditorState::WindowUiState &ui = window_ui(window_id);
    EditorCore &core = window_core(window_id);
    core.restore_view_state(ui.view_state, false);
    ui.view_state = core.view_state();
}

EditorState::PromptHistory &EditorState::active_prompt_history() {
    if (mode == Mode::Search) {
        return command_history.searches;
    }
    return command_history.get_for_kind(command_prompt_kind);
}

// Wrapper
EditorState::PromptHistory &active_prompt_history(EditorState &state) {
    return state.active_prompt_history();
}

void reset_prompt_history_navigation(EditorState::PromptHistory &history) {
    history.browse_index.reset();
    history.draft.clear();
}

void EditorState::add_prompt_history_entry(const std::u32string &entry) {
    if (entry.empty()) {
        return;
    }
    PromptHistory &history = active_prompt_history();
    if (history.entries.empty() || history.entries.back() != entry) {
        history.entries.push_back(entry);
    }
    reset_prompt_history_navigation(history);
}

void EditorState::browse_prompt_history(bool previous) {
    PromptHistory &history = active_prompt_history();
    if (history.entries.empty()) {
        return;
    }
    std::u32string &buffer = mode == Mode::Search ? search_buffer : command_buffer;
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
    prompt_cursor = buffer.size();
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

bool is_open_pair(char32_t codepoint) {
    return codepoint == U'(' || codepoint == U'[' || codepoint == U'{';
}

bool is_close_pair(char32_t codepoint) {
    return codepoint == U')' || codepoint == U']' || codepoint == U'}';
}

char32_t matching_pair_codepoint(char32_t codepoint) {
    switch (codepoint) {
        case U'(': return U')';
        case U')': return U'(';
        case U'[': return U']';
        case U']': return U'[';
        case U'{': return U'}';
        case U'}': return U'{';
        default: return U'\0';
    }
}

std::optional<char32_t> codepoint_at(const EditorCore &core, Position position) {
    if (position.row >= core.line_count()) {
        return std::nullopt;
    }
    const std::u32string &line = core.lines()[position.row];
    if (position.column >= line.size()) {
        return std::nullopt;
    }
    return line[position.column];
}

Position next_position(const EditorCore &core, Position position) {
    if (position.row >= core.line_count()) {
        return position;
    }
    const std::u32string &line = core.lines()[position.row];
    if (position.column + 1 < line.size()) {
        return {position.row, position.column + 1};
    }
    if (position.row + 1 < core.line_count()) {
        return {position.row + 1, 0};
    }
    return {core.line_count(), 0};
}

Position previous_position(const EditorCore &core, Position position) {
    if (position.row == 0 && position.column == 0) {
        return position;
    }
    if (position.row >= core.line_count()) {
        if (core.line_count() == 0) {
            return {0, 0};
        }
        std::size_t row = core.line_count() - 1;
        std::size_t length = core.line_length(row);
        return {row, length == 0 ? 0 : length - 1};
    }
    if (position.column > 0) {
        return {position.row, position.column - 1};
    }
    if (position.row == 0) {
        return {0, 0};
    }
    std::size_t row = position.row - 1;
    std::size_t length = core.line_length(row);
    return {row, length == 0 ? 0 : length - 1};
}

bool is_before_end(const EditorCore &core, Position position) {
    return position.row < core.line_count();
}

std::optional<Position> pair_position_for_cursor(const EditorCore &core) {
    Position cursor = core.cursor();
    if (std::optional<char32_t> current = codepoint_at(core, cursor); current && (is_open_pair(*current) || is_close_pair(*current))) {
        return cursor;
    }
    if (cursor.column > 0) {
        Position left{cursor.row, cursor.column - 1};
        if (std::optional<char32_t> current = codepoint_at(core, left); current && (is_open_pair(*current) || is_close_pair(*current))) {
            return left;
        }
    }
    return std::nullopt;
}

std::string make_status_bar_left_text(
    const EditorState &state,
    const EditorCore &core,
    const std::string &language,
    const std::string &workspace) {
    return mode_name(state.mode) +
           "  " + core.display_file_name() +
           "  [b " + std::to_string(state.session.index_for_buffer_id(state.active_window().buffer_id).value_or(0) + 1) +
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

void EditorState::set_status(const std::string &message) {
    status_message = message;
}

void EditorState::dispatch_editor_event(const EditorEvent &event) {
    runtime.dispatch_editor_event(event);
    lua.dispatch_editor_event(*this, event);
}

void EditorState::dispatch_editor_events(EditorCore &core) {
    EditorEvents events = core.take_events();
    for (const EditorEvent &event : events) {
        dispatch_editor_event(event);
    }
}

bool reload_editor_configuration(EditorState &state, std::string &error_message);
void handle_service_events(EditorState &state);
void begin_insert_session(EditorState &state);
void rebuild_popup_filter(EditorState &state);

void EditorState::show_popup(std::string title, std::u32string text) {
popup.visible = true;
    popup.kind = PopupKind::Text;
    popup.title = std::move(title);
    popup.text = std::move(text);
    popup.items.clear();
    popup.filter.clear();
    popup.filtered_indices.clear();
    popup.selected_index = 0;
    popup.scroll_offset = 0;
    popup.originating_mode = mode;
}

void EditorState::request_config_reload() {
    pending_config_reload = true;
}

bool EditorState::apply_pending_config_reload() {
    if (!pending_config_reload) {
        return true;
    }
    pending_config_reload = false;
    std::string error_message;
    if (reload_editor_configuration(error_message)) {
        return true;
    }
    set_status("Config reload failed: " + error_message);
    return false;
}

void show_menu_popup(
    EditorState &state,
    std::string title,
    PopupMenuItems items,
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
    state.popup.sticky = false;
    rebuild_popup_filter(state);
}

void show_key_hints_popup(
    EditorState &state,
    std::string title,
    PopupMenuItems items,
    bool sticky) {
    state.popup.visible = true;
    state.popup.kind = PopupKind::KeyHints;
    state.popup.title = std::move(title);
    state.popup.text.clear();
    state.popup.items = std::move(items);
    state.popup.filter.clear();
    state.popup.filtered_indices.clear();
    state.popup.filtered_indices.reserve(state.popup.items.size());
    for (std::size_t index = 0; index < state.popup.items.size(); ++index) {
        state.popup.filtered_indices.push_back(index);
    }
    state.popup.selected_index = 0;
    state.popup.scroll_offset = 0;
    state.popup.originating_mode = state.mode;
    state.popup.apply_target = EditorState::PopupApplyTarget::BufferText;
    state.popup.filter_mode = EditorState::PopupFilterMode::ContainsLabelOrDetail;
    state.popup.sticky = sticky;
}

void EditorState::dismiss_popup() {
popup.visible = false;
    popup.kind = PopupKind::Text;
    popup.title.clear();
    popup.text.clear();
    popup.items.clear();
    popup.filter.clear();
    popup.filtered_indices.clear();
    popup.selected_index = 0;
    popup.scroll_offset = 0;
    popup.apply_target = EditorState::PopupApplyTarget::BufferText;
    popup.filter_mode = EditorState::PopupFilterMode::ContainsLabelOrDetail;
    popup.sticky = false;
}

bool EditorState::popup_accepts_input() const {
    return popup.visible && popup.kind == PopupKind::Menu;
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
    if (!state.popup_accepts_input() || state.popup.filtered_indices.empty()) {
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
    if (!state.popup_accepts_input() || state.popup.filtered_indices.empty()) {
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
    if (!state.popup_accepts_input() || state.popup.filtered_indices.empty()) {
        return;
    }
    PopupMenuItem item = state.popup.items[state.popup.filtered_indices[state.popup.selected_index]];
    Mode originating_mode = state.popup.originating_mode;
    EditorState::PopupApplyTarget apply_target = state.popup.apply_target;
    state.dismiss_popup();
    if (apply_target == EditorState::PopupApplyTarget::CommandBuffer) {
        state.command_buffer = utf8_to_u32(item.insert_text);
        state.prompt_cursor = state.command_buffer.size();
        state.set_status(":" + item.insert_text);
        return;
    }

    EditorCore &core = state.active_core();
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
        state.sync_window_view_from_core(state.windows.active_window_id());
        state.set_status("Completion applied");
    } else {
        state.set_status("Completion failed");
    }
    if (originating_mode == Mode::Insert) {
        state.begin_insert_session();
    }
}

bool EditorState::handle_popup_input(const std::string &token) {
    if (!popup_accepts_input()) {
        return false;
    }
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);
    std::size_t visible_rows = popup_menu_visible_rows_for_screen(screen_rows);
    if (token == "up") {
        move_popup_selection(*this, false);
        ensure_popup_selection_visible(*this, visible_rows);
        return true;
    }
    if (token == "down") {
        move_popup_selection(*this, true);
        ensure_popup_selection_visible(*this, visible_rows);
        return true;
    }
    if (token == "shift-tab") {
        move_popup_selection(*this, false);
        ensure_popup_selection_visible(*this, visible_rows);
        return true;
    }
    if (token == "backspace") {
        if (!popup.filter.empty()) {
            popup.filter.pop_back();
            rebuild_popup_filter(*this);
            ensure_popup_selection_visible(*this, visible_rows);
        }
        return true;
    }
    if (popup_selection_accept_token(token)) {
        apply_popup_selection(*this);
        return true;
    }
    if (token == "esc") {
        Mode originating_mode = popup.originating_mode;
        dismiss_popup();
        if (originating_mode == Mode::Insert) {
            begin_insert_session();
        }
        set_status(mode_name(mode));
        return true;
    }
    if (token == "printable") {
        return true;
    }
    if (!token.empty() && utf8_to_u32(token).size() == 1) {
        popup.filter += utf8_to_u32(token);
        rebuild_popup_filter(*this);
        ensure_popup_selection_visible(*this, visible_rows);
        return true;
    }
    dismiss_popup();
    return false;
}

void EditorState::sync_active_window_buffer() {
if (const EditorWindow *window = windows.active_window()) {
        session.switch_to_id(window->buffer_id);
    }
}

void initialize_windows(EditorState &state) {
    state.windows = WindowManager(state.session.active_buffer_id());
    state.window_ui_map.clear();
    state.active_buffer_ui();
    state.sync_window_view_from_core(state.windows.active_window_id());
}

void EditorState::show_buffer_in_active_window(std::size_t buffer_id, bool reset_view) {
    windows.set_active_buffer_id(buffer_id);
    sync_active_window_buffer();
    if (reset_view) {
        active_buffer_ui() = EditorState::WindowUiState{};
        sync_window_view_from_core(windows.active_window_id());
    } else {
        sync_core_view_from_window(windows.active_window_id());
    }
}

void EditorState::focus_window(std::size_t window_id) {
if (!windows.set_active_window(window_id)) {
        return;
    }
    sync_active_window_buffer();
    active_buffer_ui();
    sync_core_view_from_window(window_id);
}

void invalidate_syntax_cache(EditorState &state) {
    for (auto &[buffer_id, ui_state] : state.syntax_ui_map) {
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
    log_debug("suspend_supported() check: SIGTSTP is defined, returning true");
    return true;
#else
    log_debug("suspend_supported() check: SIGTSTP is NOT defined, returning false");
    return false;
#endif
}

void EditorState::begin_insert_session() {
if (insert_session_active) {
        return;
    }
    active_core().begin_compound_edit();
    insert_session_active = true;
}

void EditorState::end_insert_session() {
if (!insert_session_active) {
        return;
    }
    active_core().end_compound_edit();
    insert_session_active = false;
}

void EditorState::enter_normal_mode(bool preserve_status) {
EditorCore &core = active_core();
    if (mode == Mode::Insert) {
        end_insert_session();
    }
    if (mode == Mode::Visual || mode == Mode::VisualLine) {
        core.clear_selection();
    }
    clear_command_selection_snapshot();
    mode = Mode::Normal;
    command_buffer.clear();
    prompt_cursor = 0;
    pending.tokens.clear();
    pending.motion = PendingMotion::None;
    pending.motion_repeat_count = 1;
    pending.replace_count = 0;
    pending.repeat_digits.clear();
    search_buffer.clear();
    Position cursor = displayed_cursor(windows.active_window_id());
    std::size_t length = core.line_length(cursor.row);
    if (cursor.column > 0 && cursor.column == length) {
        core.set_cursor({cursor.row, cursor.column - 1});
    }
    if (!preserve_status) {
        set_status(mode_name(mode));
    }
}

void EditorState::enter_insert_mode() {
clear_command_selection_snapshot();
active_core().clear_selection();
    begin_insert_session();
    mode = Mode::Insert;
    pending.tokens.clear();
    pending.motion = PendingMotion::None;
    pending.motion_repeat_count = 1;
    pending.replace_count = 0;
    pending.repeat_digits.clear();
    search_buffer.clear();
    set_status(mode_name(mode));
}

void EditorState::enter_command_mode() {
if (mode == Mode::Insert || insert_session_active) {
        end_insert_session();
    }
capture_command_selection_snapshot();
active_core().clear_selection();
    mode = Mode::Command;
    command_prompt_kind = CommandPromptKind::EditorCommand;
    command_buffer.clear();
    prompt_cursor = 0;
    reset_prompt_history_navigation(command_history.editor_commands);
    pending.tokens.clear();
    pending.motion = PendingMotion::None;
    pending.motion_repeat_count = 1;
    pending.replace_count = 0;
    pending.repeat_digits.clear();
    search_buffer.clear();
    set_status(":");
}

void EditorState::enter_filter_command_mode() {
if (!active_core().has_selection()) {
        set_status("No selection");
        return;
    }
    if (mode == Mode::Insert || insert_session_active) {
        end_insert_session();
    }
    clear_command_selection_snapshot();
    mode = Mode::Command;
    command_prompt_kind = CommandPromptKind::FilterSelection;
    command_buffer.clear();
    prompt_cursor = 0;
    reset_prompt_history_navigation(command_history.filter_commands);
    pending.tokens.clear();
    pending.motion = PendingMotion::None;
    pending.motion_repeat_count = 1;
    pending.replace_count = 0;
    pending.repeat_digits.clear();
    search_buffer.clear();
    set_status("|");
}

void EditorState::enter_sed_command_mode() {
if (!active_core().has_selection()) {
        set_status("No selection");
        return;
    }
    if (mode == Mode::Insert || insert_session_active) {
        end_insert_session();
    }
    clear_command_selection_snapshot();
    mode = Mode::Command;
    command_prompt_kind = CommandPromptKind::SedSelection;
    command_buffer.clear();
    prompt_cursor = 0;
    reset_prompt_history_navigation(command_history.sed_commands);
    pending.tokens.clear();
    pending.motion = PendingMotion::None;
    pending.motion_repeat_count = 1;
    pending.replace_count = 0;
    pending.repeat_digits.clear();
    search_buffer.clear();
    set_status("S");
}

void EditorState::enter_search_mode() {
EditorCore &core = active_core();
    EditorState::WindowUiState &window_state = active_buffer_ui();
    if (mode == Mode::Insert || insert_session_active) {
        end_insert_session();
    }
    clear_command_selection_snapshot();
    core.clear_selection();
    mode = Mode::Search;
    search_buffer.clear();
    prompt_cursor = 0;
    reset_prompt_history_navigation(command_history.searches);
    window_state.search_origin = core.cursor();
    pending.tokens.clear();
    pending.motion = PendingMotion::None;
    pending.motion_repeat_count = 1;
    pending.replace_count = 0;
    pending.repeat_digits.clear();
    set_status("/");
}

bool can_quit_without_force(const EditorState &state) {
    return !state.session.has_dirty_buffers();
}

void EditorState::quit_editor() {
should_quit = true;
}

const std::vector<DiagnosticEntryView> &sorted_diagnostics(const EditorState &state, std::size_t window_id) {
    const EditorBuffer &buffer = state.window_buffer(window_id);
    const EditorCore &core = buffer.core;
    EditorState::BufferUiState &buffer_ui = const_cast<EditorState &>(state).buffer_ui_map.try_emplace(buffer.id).first->second;
    if (buffer_ui.sorted_diagnostics_revision == core.diagnostics_revision()) {
        return buffer_ui.sorted_diagnostics;
    }

    buffer_ui.sorted_diagnostics.clear();
    const Diagnostics &source = core.diagnostics();
    buffer_ui.sorted_diagnostics.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        buffer_ui.sorted_diagnostics.push_back({index, source[index]});
    }
    std::sort(buffer_ui.sorted_diagnostics.begin(), buffer_ui.sorted_diagnostics.end(), [](const DiagnosticEntryView &left, const DiagnosticEntryView &right) {
        Range left_range = normalized_range(left.diagnostic.range);
        Range right_range = normalized_range(right.diagnostic.range);
        if ((left_range.start == right_range.start)) {
            return (left_range.end < right_range.end);
        }
        return (left_range.start < right_range.start);
    });
    buffer_ui.sorted_diagnostics_revision = core.diagnostics_revision();
    return buffer_ui.sorted_diagnostics;
}

bool EditorState::should_render_diagnostics(std::size_t window_id) const {
    return diagnostics_visible && (
        mode != Mode::Insert ||
        effective_show_diagnostics_in_insert_mode(config, window_core(window_id).file_path()));
}

void EditorState::normalize_selected_diagnostic() {
EditorState::WindowUiState &buffer_ui = active_buffer_ui();
    if (active_core().diagnostics().empty()) {
        buffer_ui.selected_diagnostic_index.reset();
        return;
    }
    if (!buffer_ui.selected_diagnostic_index || *buffer_ui.selected_diagnostic_index >= active_core().diagnostics().size()) {
        buffer_ui.selected_diagnostic_index = 0;
    }
}

void focus_diagnostic(EditorState &state, std::size_t diagnostic_index) {
    EditorState::WindowUiState &buffer_ui = state.active_buffer_ui();
    EditorCore &core = state.active_core();
    const Diagnostics &diagnostics = core.diagnostics();
    if (diagnostic_index >= diagnostics.size()) {
        return;
    }
    buffer_ui.selected_diagnostic_index = diagnostic_index;
    Range range = normalized_range(diagnostics[diagnostic_index].range);
    core.set_cursor(range.start);
}

void EditorState::show_diagnostics_summary() {
    std::size_t errors = 0;
    std::size_t warnings = 0;
    for (const Diagnostic &diagnostic : active_core().diagnostics()) {
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            ++errors;
        } else {
            ++warnings;
        }
    }
    if (errors == 0 && warnings == 0) {
        set_status("No diagnostics");
        return;
    }
    set_status(count_label(errors, "error") + ", " + count_label(warnings, "warning"));
}

void EditorState::show_lsp_status() {
    show_popup("LSP Status", utf8_to_u32(runtime.status_summary()));
    set_status("LSP status");
}

void EditorState::show_tree_sitter_status() {
    show_popup(
        "Tree-sitter Status",
        utf8_to_u32(tree_sitter_status_summary(config, active_core().file_path())));
    set_status("Tree-sitter status");
}

void EditorState::navigate_diagnostic(bool forward) {
EditorState::WindowUiState &buffer_ui = active_buffer_ui();
    const std::vector<DiagnosticEntryView> &diagnostics = sorted_diagnostics(*this, windows.active_window_id());
    if (diagnostics.empty()) {
        buffer_ui.selected_diagnostic_index.reset();
        set_status("No diagnostics");
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
    Position cursor = displayed_cursor(windows.active_window_id());
        for (std::size_t index = 0; index < diagnostics.size(); ++index) {
            Range range = normalized_range(diagnostics[index].diagnostic.range);
            if (!(range.start < cursor)) {
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

    focus_diagnostic(*this, diagnostics[current_sorted_index].index);
    const Diagnostic &diagnostic = diagnostics[current_sorted_index].diagnostic;
    set_status(
        std::string(diagnostic.severity == DiagnosticSeverity::Error ? "Error: " : "Warning: ") +
            u32_to_utf8(diagnostic.message));
}

const std::vector<AnnotationEntryView> &sorted_annotations(const EditorState &state, std::size_t window_id) {
    const EditorBuffer &buffer = state.window_buffer(window_id);
    const EditorCore &core = buffer.core;
    bool show_diagnostics = state.should_render_diagnostics(window_id);
    EditorState::BufferUiState &buffer_ui = const_cast<EditorState &>(state).buffer_ui_map.try_emplace(buffer.id).first->second;
    std::size_t combined_revision = core.annotations_revision() ^ (core.diagnostics_revision() << 1);
    auto found_cache = buffer_ui.sorted_annotations.find(show_diagnostics);
    if (found_cache != buffer_ui.sorted_annotations.end() && found_cache->second.first == combined_revision) {
        return found_cache->second.second;
    }

    std::vector<AnnotationEntryView> annotations;
    InlineAnnotations projected = core.projected_annotations();
    annotations.reserve(projected.size());

    const std::vector<DiagnosticEntryView> &diagnostics = sorted_diagnostics(state, window_id);
    for (const InlineAnnotation &annotation : projected) {
        if (annotation.kind == AnnotationKind::Diagnostic && !show_diagnostics) {
            continue;
        }
        std::optional<std::size_t> diagnostic_index;
        if (annotation.kind == AnnotationKind::Diagnostic) {
            for (const DiagnosticEntryView &diagnostic : diagnostics) {
                if ((normalized_range(diagnostic.diagnostic.range).start == normalized_range(annotation.range).start) &&
                    (normalized_range(diagnostic.diagnostic.range).end == normalized_range(annotation.range).end) &&
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
        if ((left_range.start == right_range.start)) {
            return (left_range.end < right_range.end);
        }
        return (left_range.start < right_range.start);
    });
    auto &cache_entry = buffer_ui.sorted_annotations[show_diagnostics];
    cache_entry.first = combined_revision;
    cache_entry.second = std::move(annotations);
    return cache_entry.second;
}

Lines wrap_annotation_text(const std::u32string &text, int max_cols) {
    if (max_cols <= 1) {
        return {text};
    }

    Lines wrapped;
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

VisualRows build_visual_rows(const EditorState &state, std::size_t window_id, int buffer_cols) {
    VisualRows rows;
    std::vector<AnnotationEntryView> annotations = sorted_annotations(state, window_id);
    std::size_t annotation_index = 0;

    for (std::size_t row = 0; row < state.window_core(window_id).line_count(); ++row) {
        rows.push_back({VisualRowKind::SourceLine, row, 0, std::nullopt});
        while (annotation_index < annotations.size()) {
            Range range = normalized_range(annotations[annotation_index].annotation.range);
            if (range.end.row != row) {
                break;
            }
            Lines wrapped = wrap_annotation_text(annotations[annotation_index].annotation.text, buffer_cols - 2);
            for (std::size_t wrap_index = 0; wrap_index < wrapped.size(); ++wrap_index) {
                rows.push_back({VisualRowKind::Annotation, row, wrap_index, annotations[annotation_index]});
            }
            ++annotation_index;
        }
    }

    return rows;
}

const VisualRows &visual_rows_for_window(const EditorState &state, std::size_t window_id, int buffer_cols) {
    EditorBuffer const &buffer = state.window_buffer(window_id);
    EditorState::BufferUiState &buffer_state = const_cast<EditorState &>(state).buffer_ui_map.try_emplace(buffer.id).first->second;
    const EditorCore &core = state.window_core(window_id);
    bool show_diagnostics = state.should_render_diagnostics(window_id);
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

std::size_t visual_row_for_buffer_row(const VisualRows &rows, std::size_t buffer_row) {
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].kind == VisualRowKind::SourceLine && rows[index].buffer_row == buffer_row) {
            return index;
        }
    }
    return 0;
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

void EditorState::request_definition() {
EditorCore &core = active_core();
    dispatch_editor_events(core);
    ServiceRequest request;
    request.type = ServiceRequestType::GoToDefinition;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position(core.cursor());
    runtime.dispatch_service_request(request);
    set_status("Definition requested");
}

void EditorState::request_hover() {
EditorCore &core = active_core();
    dispatch_editor_events(core);
    ServiceRequest request;
    request.type = ServiceRequestType::Hover;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position(core.cursor());
    request.document_version = core.document_version();
    runtime.dispatch_service_request(request);
    runtime.poll_services();
    handle_service_events();
    set_status("Hover requested");
}

void EditorState::request_completion() {
EditorCore &core = active_core();
    if (mode == Mode::Insert) {
        end_insert_session();
    }
    dispatch_editor_events(core);
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
    runtime.dispatch_service_request(request);
    runtime.poll_services();
    handle_service_events();
    set_status("Completion requested");
}

void request_selection_range(EditorState &state) {
    EditorCore &core = state.active_core();
    state.dispatch_editor_events(core);
    ServiceRequest request;
    request.type = ServiceRequestType::SelectionRange;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position(core.cursor());
    request.document_version = core.document_version();
    state.runtime.dispatch_service_request(request);
    state.runtime.poll_services();
    state.handle_service_events();
}

void EditorState::select_enclosing_ast() {
EditorState::WindowUiState &ui = active_buffer_ui();
    EditorCore &core = active_core();
    bool cache_valid = ui.ast_selection_document_uri == core.document_uri() &&
        ui.ast_selection_document_version == core.document_version() && !ui.ast_selection_ranges.empty();
    if (!cache_valid) {
        request_selection_range(*this);
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
        mode = Mode::Visual;
        sync_window_view_from_core(windows.active_window_id());
        ui.ast_selection_document_uri = std::move(cached_uri);
        ui.ast_selection_document_version = cached_version;
        ui.ast_selection_cursor = cached_cursor;
        ui.ast_selection_ranges = std::move(cached_ranges);
        ui.ast_selection_index = cached_index;
        set_status("Selected enclosing AST node");
    } else {
        set_status("No enclosing AST range");
    }
}

void EditorState::select_inner_ast() {
EditorState::WindowUiState &ui = active_buffer_ui();
    EditorCore &core = active_core();
    bool cache_valid = ui.ast_selection_document_uri == core.document_uri() &&
        ui.ast_selection_document_version == core.document_version() && !ui.ast_selection_ranges.empty();
    if (!cache_valid) {
        set_status("No smaller AST range");
        return;
    }
    if (ui.ast_selection_index == 0) {
        set_status("No smaller AST range");
        return;
    }
    --ui.ast_selection_index;
    std::string cached_uri = ui.ast_selection_document_uri;
    std::size_t cached_version = ui.ast_selection_document_version;
    Position cached_cursor = ui.ast_selection_cursor;
    std::vector<Range> cached_ranges = ui.ast_selection_ranges;
    std::size_t cached_index = ui.ast_selection_index;
    core.set_selection_range(ui.ast_selection_ranges[ui.ast_selection_index], SelectionMode::Character);
    mode = Mode::Visual;
    sync_window_view_from_core(windows.active_window_id());
    ui.ast_selection_document_uri = std::move(cached_uri);
    ui.ast_selection_document_version = cached_version;
    ui.ast_selection_cursor = cached_cursor;
    ui.ast_selection_ranges = std::move(cached_ranges);
    ui.ast_selection_index = cached_index;
    set_status("Selected inner AST node");
}

EditorState::JumpLocation current_jump_location(const EditorState &state) {
    const EditorCore &core = state.active_core();
    return {core.document_uri(), core.file_path(), state.displayed_cursor(state.windows.active_window_id())};
}

bool same_jump_location(const EditorState::JumpLocation &left, const EditorState::JumpLocation &right) {
    return left.document_uri == right.document_uri && (left.position == right.position);
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
        state.show_buffer_in_active_window(buffer->id);
    } else {
        if (std::optional<std::size_t> window_id = state.windows.find_window_showing_buffer(buffer->id)) {
            state.focus_window(*window_id);
        } else {
            state.show_buffer_in_active_window(buffer->id);
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
        state.set_status(empty_message);
        return;
    }

    EditorState::JumpLocation target = from_stack.back();
    from_stack.pop_back();
    EditorState::JumpLocation current = current_jump_location(state);
    if (!open_jump_location(state, target)) {
        from_stack.push_back(std::move(target));
        state.set_status("Jump target unavailable");
        return;
    }
    if (to_stack.empty() || !same_jump_location(to_stack.back(), current)) {
        to_stack.push_back(std::move(current));
    }
    state.set_status(success_message);
}

bool EditorState::reload_editor_configuration(std::string &error_message) {
    try {
        EditorConfig new_config = load_editor_config();
        KeyBindings new_keybindings = load_keybindings(new_config);
        Theme new_theme = load_theme(new_config);

        config = std::move(new_config);
        configure_logger(config.log_path);
        log_debug("reload-config applied");
        session.configure_clipboard(config.clipboard);
        keybindings = std::move(new_keybindings);
        theme = std::move(new_theme);
        invalidate_syntax_runtime_cache();
        invalidate_syntax_cache(*this);
        apply_theme_to_terminal(theme);
        clearok(stdscr, TRUE);
        refresh();
        runtime.clear_services();
        if (!config.lsp_servers.empty()) {
            for (const LspServerConfig &server : config.lsp_servers) {
                runtime.add_service(std::make_unique<LspService>(server));
            }
        }
        if (!config.lsp_servers.empty()) {
            runtime.start_services();
            for (EditorBuffer &buffer : session.buffers()) {
                if (buffer.core.document_uri().empty()) {
                    continue;
                }
                dispatch_editor_event({
                    EditorEventType::DocumentOpened,
                    buffer.core.document_uri(),
                    buffer.core.document_version(),
                    buffer.core.cursor(),
                    std::nullopt,
                    utf8_to_u32(buffer_text_utf8(buffer)),
                });
            }
        }
        std::string lua_error;
        if (!lua.initialize(*this, config.lua_path, lua_error)) {
            error_message = lua_error;
            return false;
        }
        return true;
    } catch (const std::exception &error) {
        error_message = error.what();
        return false;
    }
}

void EditorState::set_search_status(const std::string &suffix) {
    std::string prompt = "/" + u32_to_utf8(search_buffer);
    if (!suffix.empty()) {
        prompt += "  " + suffix;
    }
    set_status(prompt);
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
    EditorState::BufferUiState &buffer_ui = state.active_buffer_cache();
    const EditorCore &core = state.active_core();
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

    EditorState::BufferUiState &buffer_ui = state.buffer_ui_state(state.window_buffer(window_id).id);
    const EditorCore &core = state.window_core(window_id);
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

void EditorState::refresh_search_matches(bool move_to_best_match) {
EditorState::WindowUiState &window_state = active_buffer_ui();
    EditorState::BufferUiState &buffer_ui = active_buffer_cache();
    EditorCore &core = active_core();
    if (buffer_ui.search_matches_version != core.document_version() ||
        buffer_ui.compiled_search_pattern_utf8 != u32_to_utf8(buffer_ui.active_search_pattern)) {
        rebuild_search_matches(*this);
    }

    if (!buffer_ui.search_pattern_valid || buffer_ui.search_matches.empty()) {
        if (move_to_best_match) {
            window_state.current_search_match_index.reset();
        }
        return;
    }

    Position anchor = move_to_best_match ? window_state.search_origin : displayed_cursor(windows.active_window_id());
    std::size_t best_index = 0;
    bool found_best = false;
    for (std::size_t index = 0; index < buffer_ui.search_matches.size(); ++index) {
        if (!(buffer_ui.search_matches[index].start < anchor)) {
            best_index = index;
            found_best = true;
            break;
        }
    }
    window_state.current_search_match_index = found_best ? best_index : 0;
    if (move_to_best_match) {
        core.set_cursor(buffer_ui.search_matches[*window_state.current_search_match_index].start);
        sync_window_view_from_core(windows.active_window_id());
    }
}

void EditorState::refresh_search_matches_for_window(std::size_t window_id) {
    BufferUiState &buffer_ui = buffer_ui_state(window_buffer(window_id).id);
    EditorCore &core = window_core(window_id);
    if (buffer_ui.search_matches_version != core.document_version() ||
        buffer_ui.compiled_search_pattern_utf8 != u32_to_utf8(buffer_ui.active_search_pattern)) {
        rebuild_search_matches(*this, window_id);
    }
}

void EditorState::navigate_search_match(bool forward) {
EditorState::WindowUiState &window_state = active_buffer_ui();
    EditorState::BufferUiState &buffer_ui = active_buffer_cache();
    EditorCore &core = active_core();
    if (buffer_ui.search_matches.empty()) {
        set_status(buffer_ui.search_pattern_valid ? "No search matches" : "invalid regex");
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
    sync_window_view_from_core(windows.active_window_id());
    set_status(forward ? "Next match" : "Previous match");
}

std::optional<Position> matching_pair_cursor(const EditorCore &core) {
    return pair_position_for_cursor(core);
}

std::optional<Position> matching_pair_position(const EditorCore &core, Position pair_position) {
    std::optional<char32_t> codepoint = codepoint_at(core, pair_position);
    if (!codepoint) {
        return std::nullopt;
    }

    char32_t target = matching_pair_codepoint(*codepoint);
    if (target == U'\0') {
        return std::nullopt;
    }

    int depth = 0;
    if (is_open_pair(*codepoint)) {
        for (Position position = next_position(core, pair_position); is_before_end(core, position); position = next_position(core, position)) {
            std::optional<char32_t> current = codepoint_at(core, position);
            if (!current) {
                continue;
            }
            if (*current == *codepoint) {
                ++depth;
            } else if (*current == target) {
                if (depth == 0) {
                    return position;
                }
                --depth;
            }
        }
        return std::nullopt;
    }

    for (Position position = previous_position(core, pair_position); !(position == pair_position); position = previous_position(core, position)) {
        std::optional<char32_t> current = codepoint_at(core, position);
        if (current) {
            if (*current == *codepoint) {
                ++depth;
            } else if (*current == target) {
                if (depth == 0) {
                    return position;
                }
                --depth;
            }
        }
        if (position.row == 0 && position.column == 0) {
            break;
        }
    }
    return std::nullopt;
}

bool jump_to_matching_pair(EditorState &state, bool extend_selection) {
    EditorCore &core = state.active_core();
    std::optional<Position> pair = pair_position_for_cursor(core);
    if (!pair) {
        state.set_status("No matching pair");
        return false;
    }

    std::optional<Position> match = matching_pair_position(core, *pair);
    if (!match) {
        state.set_status("Unmatched pair");
        return false;
    }

    if (extend_selection && state.mode == Mode::Normal) {
        state.mode = Mode::Visual;
        core.begin_selection(SelectionMode::Character);
    }
    core.set_cursor(*match);
    state.sync_window_view_from_core(state.windows.active_window_id());
    state.set_status("Matching pair");
    return true;
}
