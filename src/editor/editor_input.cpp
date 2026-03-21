#include "editor_internal.hpp"

#include "command_recording.hpp"
#include "logger.hpp"

#ifdef _WIN32
#include "pdcurses_compat.hpp"
#endif

namespace {

void log_input_token(const EditorState &state, const std::string &token, wint_t key, bool printable, bool is_special) {
    log_debug(
        "input token=" + token +
        " key=" + std::to_string(static_cast<long long>(key)) +
        " printable=" + std::string(printable ? "true" : "false") +
        " special=" + std::string(is_special ? "true" : "false") +
        " mode=" + mode_name(state.mode));
}

}  // namespace

std::u32string &active_prompt_buffer(EditorState &state) {
    return state.mode == Mode::Search ? state.search_buffer : state.command_buffer;
}

void move_prompt_cursor(EditorState &state, std::size_t cursor) {
    std::u32string &buffer = active_prompt_buffer(state);
    state.prompt_cursor = std::min(cursor, buffer.size());
}

bool handle_prompt_token(EditorState &state, const std::string &token, wint_t key, bool printable) {
    if (state.mode != Mode::Command && state.mode != Mode::Search) {
        return false;
    }

    if (state.mode == Mode::Command) {
        if (token == "esc") {
            state.enter_normal_mode();
            return true;
        }
        if (token == "enter") {
            state.execute_command();
            return true;
        }
        if (token == "tab") {
            state.show_command_completion();
            return true;
        }
        if (token == "backspace") {
            if (state.prompt_cursor > 0) {
                state.command_buffer.erase(state.prompt_cursor - 1, 1);
                --state.prompt_cursor;
            }
            return true;
        }
        if (token == "left") {
            if (state.prompt_cursor > 0) {
                --state.prompt_cursor;
            }
            return true;
        }
        if (token == "right") {
            move_prompt_cursor(state, state.prompt_cursor + 1);
            return true;
        }
        if (token == "home") {
            state.prompt_cursor = 0;
            return true;
        }
        if (token == "end") {
            move_prompt_cursor(state, std::numeric_limits<std::size_t>::max());
            return true;
        }
        if (token == "up") {
            state.browse_prompt_history(true);
            return true;
        }
        if (token == "down") {
            state.browse_prompt_history(false);
            return true;
        }
        if (printable) {
            state.command_buffer.insert(
                state.command_buffer.begin() + static_cast<std::ptrdiff_t>(state.prompt_cursor),
                static_cast<char32_t>(key));
            ++state.prompt_cursor;
            return true;
        }
        return false;
    }

    EditorState::BufferUiState &buffer_state = state.active_buffer_cache();
    if (token == "esc") {
        state.enter_normal_mode();
        return true;
    }
    if (token == "enter") {
        state.add_prompt_history_entry(state.search_buffer);
        buffer_state.active_search_pattern = state.search_buffer;
        state.refresh_search_matches(true);
        if (!buffer_state.search_pattern_valid) {
            state.set_search_status("invalid regex");
            return true;
        }
        state.mode = Mode::Normal;
        state.set_status(buffer_state.search_matches.empty() ? "No search matches" : "Search applied");
        return true;
    }
    if (token == "backspace") {
        if (state.prompt_cursor > 0) {
            state.search_buffer.erase(state.prompt_cursor - 1, 1);
            --state.prompt_cursor;
        }
    } else if (token == "left") {
        if (state.prompt_cursor > 0) {
            --state.prompt_cursor;
        }
    } else if (token == "right") {
        move_prompt_cursor(state, state.prompt_cursor + 1);
    } else if (token == "home") {
        state.prompt_cursor = 0;
    } else if (token == "end") {
        move_prompt_cursor(state, std::numeric_limits<std::size_t>::max());
    } else if (token == "up") {
        state.browse_prompt_history(true);
    } else if (token == "down") {
        state.browse_prompt_history(false);
    } else if (printable) {
        state.search_buffer.insert(
            state.search_buffer.begin() + static_cast<std::ptrdiff_t>(state.prompt_cursor),
            static_cast<char32_t>(key));
        ++state.prompt_cursor;
    } else {
        return false;
    }

    buffer_state.active_search_pattern = state.search_buffer;
    state.refresh_search_matches(true);
    state.set_search_status(buffer_state.search_pattern_valid ? "" : "invalid regex");
    return true;
}

void append_after_cursor(EditorState &state) {
    EditorCore &core = state.active_core();
    Position cursor = core.cursor();
    std::size_t length = core.line_length(cursor.row);
    if (cursor.column < length) {
        core.set_cursor({cursor.row, cursor.column + 1});
    }
    state.enter_insert_mode();
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

bool EditorState::split_active_window(WindowSplitDirection direction) {
    if (!windows.split_active(direction)) {
        return false;
    }
    sync_active_window_buffer();
    active_buffer_ui();
    sync_window_view_from_core(windows.active_window_id());
    return true;
}

bool EditorState::close_active_window() {
    if (windows.window_count() <= 1) {
        if (!can_quit_without_force(*this)) {
            set_status("Unsaved changes; use :q! to quit");
            return false;
        }
        quit_editor();
        return true;
    }

    std::size_t closing_window_id = windows.active_window_id();
    if (!windows.close_active()) {
        return false;
    }
    if (panel_window_id && *panel_window_id == closing_window_id) {
        panel_window_id.reset();
    }
    window_ui_map.erase(closing_window_id);
    sync_active_window_buffer();
    active_buffer_ui();
    return true;
}

bool EditorState::close_other_windows() {
    if (!windows.close_others()) {
        return false;
    }
    std::size_t active_window_id = windows.active_window_id();
    if (panel_window_id && *panel_window_id != active_window_id) {
        panel_window_id.reset();
    }
    auto kept = window_ui_map.find(active_window_id);
    if (kept == window_ui_map.end()) {
        window_ui_map.clear();
    } else {
        WindowUiState kept_state = std::move(kept->second);
        window_ui_map.clear();
        window_ui_map.emplace(active_window_id, std::move(kept_state));
    }
    sync_active_window_buffer();
    active_buffer_ui();
    return true;
}

bool ensure_active_buffer_editable(EditorState &state) {
    if (state.active_buffer_is_user_editable()) {
        return true;
    }
    state.enter_normal_mode(true);
    state.set_status("Buffer is read-only");
    return false;
}

bool focus_window_direction(EditorState &state, WindowMoveDirection direction) {
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);
    if (!state.windows.focus_direction(direction, screen_rows, screen_cols, 2)) {
        return false;
    }
    state.sync_active_window_buffer();
    state.active_buffer_ui();
    state.sync_core_view_from_window(state.windows.active_window_id());
    return true;
}

void page_up(EditorState &state) {
    state.active_core().move_by_lines(-page_step());
    state.sync_window_view_from_core(state.windows.active_window_id());
}

void page_down(EditorState &state) {
    state.active_core().move_by_lines(page_step());
    state.sync_window_view_from_core(state.windows.active_window_id());
}

void half_page_up(EditorState &state) {
    state.active_core().move_by_lines(-half_page_step());
    state.sync_window_view_from_core(state.windows.active_window_id());
}

void half_page_down(EditorState &state) {
    state.active_core().move_by_lines(half_page_step());
    state.sync_window_view_from_core(state.windows.active_window_id());
}

void enter_visual_mode(EditorState &state) {
    state.mode = Mode::Visual;
    state.pending.tokens.clear();
    state.pending.motion = PendingMotion::None;
    state.active_core().begin_selection(SelectionMode::Character);
    state.set_status(mode_name(state.mode));
}

void enter_visual_line_mode(EditorState &state) {
    state.mode = Mode::VisualLine;
    state.pending.tokens.clear();
    state.pending.motion = PendingMotion::None;
    state.active_core().begin_selection(SelectionMode::Line);
    state.set_status(mode_name(state.mode));
}

std::size_t take_repeat_count(EditorState &state);

void prepare_visual_motion(EditorState &state) {
    if (state.mode != Mode::Normal) {
        return;
    }
    state.mode = Mode::Visual;
    state.pending.tokens.clear();
    state.pending.motion = PendingMotion::None;
    state.active_core().begin_selection(SelectionMode::Character);
    state.set_status(mode_name(state.mode));
}

void begin_pending_motion(EditorState &state, PendingMotion motion, const char *status) {
    state.pending.motion_repeat_count = take_repeat_count(state);
    state.pending.motion = motion;
    state.set_status(status);
}

bool select_entire_buffer(EditorState &state) {
    EditorCore &core = state.active_core();
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
    state.set_status(mode_name(state.mode));
    return true;
}

bool move_cursor_to_selection_boundary(EditorState &state, bool to_end) {
    EditorCore &core = state.active_core();
    std::optional<Range> selection = core.selection_range();
    if (!selection) {
        return false;
    }

    SelectionMode mode = core.selection_mode();
    Position start = selection->start;
    Position character_end = selection->end;
    if (character_end.column > 0) {
        --character_end.column;
    } else if (character_end.row > 0) {
        --character_end.row;
        character_end.column = core.line_length(character_end.row);
    }
    Position end = mode == SelectionMode::Line
        ? Position{
              selection->end.column == 0 && selection->end.row > selection->start.row ? selection->end.row - 1
                                                                                       : selection->end.row,
              0}
        : character_end;

    EditorViewState view_state = core.view_state();
    view_state.selection_mode = mode;
    view_state.cursor = to_end ? end : start;
    view_state.selection_anchor = to_end ? std::optional<Position>(start) : std::optional<Position>(end);
    core.restore_view_state(view_state, true);
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
    EditorCore &core = state.active_core();
    auto [start_row, end_row] = selected_line_span(core);
    std::size_t shiftwidth = effective_shiftwidth(state.config, core.file_path());
    std::size_t tabstop = effective_tabstop(state.config, core.file_path());
    bool expandtab = effective_expandtab(state.config, core.file_path());
    std::optional<Range> selection = core.selection_range();
    SelectionMode selection_mode = core.selection_mode();
    bool changed = indent
        ? core.indent_lines(start_row, end_row, shiftwidth, expandtab, tabstop)
        : core.outdent_lines(start_row, end_row, shiftwidth, tabstop);
    if (changed && selection && (state.mode == Mode::Visual || state.mode == Mode::VisualLine)) {
        core.set_selection_range(*selection, selection_mode);
    }
    return changed;
}

bool motion_is_character_based(PendingMotion motion) {
    return motion != PendingMotion::None;
}

bool execute_pending_motion(EditorState &state, char32_t target) {
    EditorCore &core = state.active_core();
    bool moved = false;
    for (std::size_t attempt = 0; attempt < state.pending.motion_repeat_count; ++attempt) {
        bool step_moved = false;
        switch (state.pending.motion) {
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
    state.pending.motion = PendingMotion::None;
    state.pending.motion_repeat_count = 1;
    state.pending.tokens.clear();
    state.set_status(moved ? mode_name(state.mode) : "Target not found");
    return moved;
}

std::optional<std::string> key_token(wint_t key, bool is_special) {
    if (is_special) {
        switch (key) {
            case KEY_LEFT:
                return "left";
            case KEY_SLEFT:
                return "shift-left";
            case KEY_RIGHT:
                return "right";
            case KEY_SRIGHT:
                return "shift-right";
            case KEY_UP:
                return "up";
            case KEY_SR:
                return "shift-up";
            case KEY_DOWN:
                return "down";
            case KEY_SF:
                return "shift-down";
            case KEY_HOME:
                return "home";
            case KEY_END:
                return "end";
#ifdef KEY_SHOME
            case KEY_SHOME:
                return "shift-home";
#endif
#ifdef KEY_SEND
            case KEY_SEND:
                return "shift-end";
#endif
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

std::string joined_key_sequence(const std::vector<std::string> &tokens) {
    std::string result;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (!result.empty()) {
            result += ' ';
        }
        result += tokens[index];
    }
    return result;
}

std::string key_hint_popup_title(const EditorState &state) {
    if (state.pending.tokens.empty()) {
        return mode_name(state.mode) + " keys";
    }
    return mode_name(state.mode) + " after " + joined_key_sequence(state.pending.tokens);
}

PopupMenuItems key_hint_popup_items(const EditorState &state) {
    std::vector<KeyHint> hints = key_hints_for_prefix(state.keybindings, mode_key(state), state.pending.tokens);
    PopupMenuItems items;
    items.reserve(hints.size());
    for (const KeyHint &hint : hints) {
        items.push_back({hint.token, hint.detail, {}, std::nullopt});
    }
    return items;
}

bool popup_is_key_hints(const EditorState &state) {
    return state.popup.visible && state.popup.kind == PopupKind::KeyHints;
}

void refresh_key_hint_popup(EditorState &state, bool sticky) {
    PopupMenuItems items = key_hint_popup_items(state);
    if (items.empty()) {
        state.dismiss_popup();
        return;
    }
    show_key_hints_popup(state, key_hint_popup_title(state), std::move(items), sticky);
}

void sync_key_hint_popup(EditorState &state) {
    if (!popup_is_key_hints(state)) {
        return;
    }
    refresh_key_hint_popup(state, state.popup.sticky);
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
    return mode_supports_command_language(state) && state.pending.token_starts_repeat(token);
}

std::size_t current_repeat_count(const EditorState &state) {
    return state.pending.current_repeat_count();
}

std::size_t take_repeat_count(EditorState &state) {
    return state.pending.take_repeat_count();
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
    if (token == "shift-left") {
        return KEY_SLEFT;
    }
    if (token == "shift-right") {
        return KEY_SRIGHT;
    }
    if (token == "shift-up") {
        return KEY_SR;
    }
    if (token == "shift-down") {
        return KEY_SF;
    }
    if (token == "home") {
        return KEY_HOME;
    }
    if (token == "end") {
        return KEY_END;
    }
#ifdef KEY_SHOME
    if (token == "shift-home") {
        return KEY_SHOME;
    }
#endif
#ifdef KEY_SEND
    if (token == "shift-end") {
        return KEY_SEND;
    }
#endif
    std::u32string text = utf8_to_u32(token);
    if (text.size() == 1) {
        return static_cast<wint_t>(text[0]);
    }
    return std::nullopt;
}

bool token_is_printable_for_replay(const std::string &token) {
    if (token == "esc" || token == "enter" || token == "backspace" || token == "left" || token == "right" ||
        token == "up" || token == "down" || token == "shift-left" || token == "shift-right" ||
        token == "shift-up" || token == "shift-down" || token == "home" || token == "shift-home" ||
        token == "end" || token == "shift-end" || token == "pageup" || token == "pagedown" ||
        token == "tab" || token == "shift-tab") {
        return false;
    }
    if (token.starts_with("ctrl-")) {
        return false;
    }
    return true;
}

void execute_dispatch(EditorState &state, const KeyDispatch &dispatch, wint_t key);
void process_input_token(EditorState &state, const std::string &token, wint_t key, bool printable);

void push_back_keys(const std::vector<wint_t> &keys) {
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        unget_wch(*it);
    }
}

std::optional<std::string> shift_arrow_token_from_escape(const std::vector<wint_t> &keys) {
    if (keys.size() != 5 || keys[0] != '[' || keys[1] != '1' || keys[2] != ';' || keys[3] != '2') {
        return std::nullopt;
    }
    switch (keys[4]) {
        case 'A':
            return "shift-up";
        case 'B':
            return "shift-down";
        case 'C':
            return "shift-right";
        case 'D':
            return "shift-left";
        default:
            return std::nullopt;
    }
}

std::optional<std::string> shift_home_end_token_from_escape(const std::vector<wint_t> &keys) {
    if (keys.size() != 5 || keys[0] != '[' || keys[1] != '1' || keys[2] != ';' || keys[3] != '2') {
        return std::nullopt;
    }
    switch (keys[4]) {
        case 'H':
            return "shift-home";
        case 'F':
            return "shift-end";
        default:
            return std::nullopt;
    }
}

void execute_expansion(EditorState &state, const std::vector<std::string> &expansion) {
    static constexpr std::size_t kMaxExpansionDepth = 16;
    if (state.recording.replay_depth >= kMaxExpansionDepth) {
        state.set_status("Keybinding expansion too deep");
        state.pending.tokens.clear();
        return;
    }

    ++state.recording.replay_depth;
    for (const std::string &token : expansion) {
        std::optional<wint_t> key = key_from_token(token);
        process_input_token(state, token, key.value_or(0), token_is_printable_for_replay(token));
    }
    --state.recording.replay_depth;
}

bool action_accepts_repeat(EditorAction action) {
    switch (action) {
        case EditorAction::MoveLeft:
        case EditorAction::VisualMoveLeft:
        case EditorAction::MoveRight:
        case EditorAction::VisualMoveRight:
        case EditorAction::MoveUp:
        case EditorAction::VisualMoveUp:
        case EditorAction::MoveDown:
        case EditorAction::VisualMoveDown:
        case EditorAction::MoveLineStart:
        case EditorAction::VisualMoveLineStart:
        case EditorAction::MoveLineEnd:
        case EditorAction::VisualMoveLineEnd:
        case EditorAction::DeleteChar:
        case EditorAction::ReplaceChar:
        case EditorAction::Undo:
        case EditorAction::Redo:
        case EditorAction::PasteAfter:
        case EditorAction::PasteBefore:
        case EditorAction::GotoTop:
        case EditorAction::VisualGotoTop:
        case EditorAction::GotoBottom:
        case EditorAction::VisualGotoBottom:
        case EditorAction::DeleteLine:
        case EditorAction::HalfPageDown:
        case EditorAction::HalfPageUp:
        case EditorAction::PageUp:
        case EditorAction::PageDown:
        case EditorAction::Indent:
        case EditorAction::Outdent:
        case EditorAction::SearchNext:
        case EditorAction::VisualSearchNext:
        case EditorAction::SearchPrevious:
        case EditorAction::VisualSearchPrevious:
        case EditorAction::NextDiagnostic:
        case EditorAction::PreviousDiagnostic:
            return true;
        default:
            return false;
    }
}

bool action_defers_completion(EditorAction action) {
    return action == EditorAction::FindForward || action == EditorAction::FindBackward ||
        action == EditorAction::TillForward || action == EditorAction::TillBackward ||
        action == EditorAction::VisualFindForward || action == EditorAction::VisualFindBackward ||
        action == EditorAction::VisualTillForward || action == EditorAction::VisualTillBackward;
}

bool action_handles_own_repeat(EditorAction action) {
    return action == EditorAction::RepeatLastCommand;
}

bool handle_motion_action(EditorState &state, EditorAction action) {
    EditorCore &core = state.active_core();
    switch (action) {
        case EditorAction::MoveLeft:
            core.move_left();
            return true;
        case EditorAction::VisualMoveLeft:
            prepare_visual_motion(state);
            core.move_left();
            return true;
        case EditorAction::MoveRight:
            core.move_right();
            return true;
        case EditorAction::VisualMoveRight:
            prepare_visual_motion(state);
            core.move_right();
            return true;
        case EditorAction::MoveUp:
            core.move_up();
            return true;
        case EditorAction::VisualMoveUp:
            prepare_visual_motion(state);
            core.move_up();
            return true;
        case EditorAction::MoveDown:
            core.move_down();
            return true;
        case EditorAction::VisualMoveDown:
            prepare_visual_motion(state);
            core.move_down();
            return true;
        case EditorAction::MoveLineStart:
            core.move_line_start();
            return true;
        case EditorAction::VisualMoveLineStart:
            prepare_visual_motion(state);
            core.move_line_start();
            return true;
        case EditorAction::MoveLineEnd:
            core.move_line_end();
            return true;
        case EditorAction::VisualMoveLineEnd:
            prepare_visual_motion(state);
            core.move_line_end();
            return true;
        case EditorAction::FindForward:
            begin_pending_motion(state, PendingMotion::FindForward, "f");
            return true;
        case EditorAction::VisualFindForward:
            prepare_visual_motion(state);
            begin_pending_motion(state, PendingMotion::FindForward, "F");
            return true;
        case EditorAction::FindBackward:
            begin_pending_motion(state, PendingMotion::FindBackward, "gf");
            return true;
        case EditorAction::VisualFindBackward:
            prepare_visual_motion(state);
            begin_pending_motion(state, PendingMotion::FindBackward, "gF");
            return true;
        case EditorAction::TillForward:
            begin_pending_motion(state, PendingMotion::TillForward, "t");
            return true;
        case EditorAction::VisualTillForward:
            prepare_visual_motion(state);
            begin_pending_motion(state, PendingMotion::TillForward, "T");
            return true;
        case EditorAction::TillBackward:
            begin_pending_motion(state, PendingMotion::TillBackward, "gt");
            return true;
        case EditorAction::VisualTillBackward:
            prepare_visual_motion(state);
            begin_pending_motion(state, PendingMotion::TillBackward, "gT");
            return true;
        case EditorAction::GotoTop:
            core.move_to_first_line();
            core.move_line_start();
            state.set_status("Top of file");
            return true;
        case EditorAction::VisualGotoTop:
            prepare_visual_motion(state);
            core.move_to_first_line();
            core.move_line_start();
            state.set_status("Top of file");
            return true;
        case EditorAction::GotoBottom:
            core.move_to_last_line();
            core.move_line_start();
            state.set_status("Bottom of file");
            return true;
        case EditorAction::VisualGotoBottom:
            prepare_visual_motion(state);
            core.move_to_last_line();
            core.move_line_start();
            state.set_status("Bottom of file");
            return true;
        case EditorAction::HalfPageDown:
            half_page_down(state);
            return true;
        case EditorAction::HalfPageUp:
            half_page_up(state);
            return true;
        case EditorAction::PageUp:
            page_up(state);
            return true;
        case EditorAction::PageDown:
            page_down(state);
            return true;
        case EditorAction::SearchNext:
            state.refresh_search_matches(false);
            state.navigate_search_match(true);
            return true;
        case EditorAction::VisualSearchNext:
            prepare_visual_motion(state);
            state.refresh_search_matches(false);
            state.navigate_search_match(true);
            return true;
        case EditorAction::SearchPrevious:
            state.refresh_search_matches(false);
            state.navigate_search_match(false);
            return true;
        case EditorAction::VisualSearchPrevious:
            prepare_visual_motion(state);
            state.refresh_search_matches(false);
            state.navigate_search_match(false);
            return true;
        case EditorAction::NextDiagnostic:
            state.navigate_diagnostic(true);
            return true;
        case EditorAction::PreviousDiagnostic:
            state.navigate_diagnostic(false);
            return true;
        case EditorAction::JumpToMatchingPair:
            jump_to_matching_pair(state, false);
            return true;
        case EditorAction::VisualJumpToMatchingPair:
            jump_to_matching_pair(state, true);
            return true;
        default:
            return false;
    }
}

bool handle_mode_and_insert_action(EditorState &state, EditorAction action, wint_t key) {
    EditorCore &core = state.active_core();
    switch (action) {
        case EditorAction::EnterInsertMode:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.enter_insert_mode();
            return true;
        case EditorAction::AppendAfterCursor:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            append_after_cursor(state);
            return true;
        case EditorAction::EnterVisualMode:
            enter_visual_mode(state);
            return true;
        case EditorAction::EnterVisualLineMode:
            enter_visual_line_mode(state);
            return true;
        case EditorAction::AppendLineEndInsert: {
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            Position cursor = core.cursor();
            core.set_cursor({cursor.row, core.line_length(cursor.row)});
            state.enter_insert_mode();
            return true;
        }
        case EditorAction::InsertLineStartInsert: {
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            Position cursor = core.cursor();
            core.set_cursor({cursor.row, 0});
            state.enter_insert_mode();
            return true;
        }
        case EditorAction::OpenLineBelow:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.begin_insert_session();
            if (effective_autoindent(state.config, core.file_path())) {
                core.open_line_below_with_autoindent();
            } else {
                core.open_line_below();
            }
            state.mode = Mode::Insert;
            state.set_status(mode_name(state.mode));
            return true;
        case EditorAction::OpenLineAbove:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.begin_insert_session();
            core.open_line_above();
            state.mode = Mode::Insert;
            state.set_status(mode_name(state.mode));
            return true;
        case EditorAction::EnterCommandMode:
            state.enter_command_mode();
            return true;
        case EditorAction::EnterSearchMode:
            state.enter_search_mode();
            state.refresh_search_matches(true);
            state.set_search_status();
            return true;
        case EditorAction::EnterNormalMode:
            state.enter_normal_mode();
            return true;
        case EditorAction::InsertNewline:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.begin_insert_session();
            if (effective_autoindent(state.config, core.file_path())) {
                core.insert_newline_with_autoindent();
            } else {
                core.insert_newline();
            }
            return true;
        case EditorAction::InsertSoftTab:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.begin_insert_session();
            core.insert_soft_tab(
                effective_tabstop(state.config, core.file_path()),
                effective_softtabstop(state.config, core.file_path()),
                effective_expandtab(state.config, core.file_path()));
            return true;
        case EditorAction::InsertOutdent:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.begin_insert_session();
            core.outdent_before_cursor(
                effective_tabstop(state.config, core.file_path()),
                effective_softtabstop(state.config, core.file_path()));
            return true;
        case EditorAction::Backspace:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.begin_insert_session();
            core.backspace_character();
            return true;
        case EditorAction::SelfInsert:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.begin_insert_session();
            core.insert_codepoint(static_cast<char32_t>(key));
            return true;
        default:
            return false;
    }
}

bool handle_edit_action(EditorState &state, EditorAction action) {
    EditorCore &core = state.active_core();
    switch (action) {
        case EditorAction::DeleteChar:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            core.delete_character_under_cursor();
            state.set_status("Deleted character");
            return true;
        case EditorAction::ReplaceChar:
            state.pending.replace_count = take_repeat_count(state);
            state.set_status("r");
            return true;
        case EditorAction::RepeatLastCommand:
            state.recording.recording_nonrepeatable = true;
            state.recording.repeat_last_command(state);
            return true;
        case EditorAction::Undo:
            state.recording.recording_nonrepeatable = true;
            state.set_status(core.undo() ? "Undid change" : "Nothing to undo");
            return true;
        case EditorAction::Redo:
            state.recording.recording_nonrepeatable = true;
            state.set_status(core.redo() ? "Redid change" : "Nothing to redo");
            return true;
        case EditorAction::PasteAfter:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.session.sync_active_clipboard();
            state.set_status(core.paste_after_cursor() ? "Pasted" : "Yank buffer empty");
            return true;
        case EditorAction::PasteBefore:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.session.sync_active_clipboard();
            state.set_status(core.paste_before_cursor() ? "Pasted" : "Yank buffer empty");
            return true;
        case EditorAction::DeleteLine:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            core.delete_current_line();
            state.set_status("Deleted line");
            return true;
        case EditorAction::Indent:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.set_status(indent_selection_or_line(state, true) ? "Indented" : "Nothing to indent");
            return true;
        case EditorAction::Outdent:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.set_status(indent_selection_or_line(state, false) ? "Outdented" : "Nothing to outdent");
            return true;
        case EditorAction::DeleteSelection: {
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            SelectionMode selection_mode = core.selection_mode();
            if (core.delete_selection()) {
                state.session.capture_active_clipboard();
                state.mode = Mode::Normal;
                state.set_status(selection_mode == SelectionMode::Line ? "Deleted lines" : "Deleted selection");
            } else {
                state.set_status("No selection");
            }
            return true;
        }
        case EditorAction::FilterSelection:
            state.enter_filter_command_mode();
            return true;
        case EditorAction::SedSelection:
            state.enter_sed_command_mode();
            return true;
        case EditorAction::YankSelection:
            if (core.yank_selection()) {
                SelectionMode selection_mode = core.selection_mode();
                state.session.capture_active_clipboard();
                core.clear_selection();
                state.mode = Mode::Normal;
                state.set_status(selection_mode == SelectionMode::Line ? "Yanked lines" : "Yanked selection");
            } else {
                state.set_status("No selection");
            }
            return true;
        case EditorAction::ChangeSelection:
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.begin_insert_session();
            if (core.delete_selection()) {
                state.session.capture_active_clipboard();
                state.enter_insert_mode();
            } else {
                state.end_insert_session();
                state.set_status("No selection");
            }
            return true;
        case EditorAction::ReplaceSelectionWithYank: {
            if (!ensure_active_buffer_editable(state)) {
                return true;
            }
            state.session.sync_active_clipboard();
            SelectionMode selection_mode = core.selection_mode();
            if (core.replace_selection_with_yank()) {
                state.mode = Mode::Normal;
                state.set_status(selection_mode == SelectionMode::Line ? "Replaced lines" : "Replaced selection");
            } else {
                state.set_status("Yank buffer empty");
            }
            return true;
        }
        case EditorAction::SelectAll:
            if (!select_entire_buffer(state)) {
                state.set_status("Selection empty");
            }
            return true;
        case EditorAction::MoveToSelectionStart:
            if (!move_cursor_to_selection_boundary(state, false)) {
                state.set_status("No selection");
            }
            return true;
        case EditorAction::MoveToSelectionEnd:
            if (!move_cursor_to_selection_boundary(state, true)) {
                state.set_status("No selection");
            }
            return true;
        case EditorAction::SelectInnerWord:
            if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
                std::optional<Range> range = core.inner_word_range();
                if (range && core.extend_selection_to_range(*range)) {
                    state.mode = Mode::Visual;
                    state.set_status("VISUAL");
                } else {
                    state.set_status("No word");
                }
            }
            return true;
        case EditorAction::SelectAroundWord:
            if (state.mode == Mode::Visual || state.mode == Mode::VisualLine) {
                std::optional<Range> range = core.a_word_range();
                if (range && core.extend_selection_to_range(*range)) {
                    state.mode = Mode::Visual;
                    state.set_status("VISUAL");
                } else {
                    state.set_status("No word");
                }
            }
            return true;
        default:
            return false;
    }
}

bool handle_window_action(EditorState &state, EditorAction action) {
    switch (action) {
        case EditorAction::NextBuffer:
            state.session.switch_to_id(state.active_window().buffer_id);
            state.session.next_buffer();
            state.show_buffer_in_active_window(state.session.active_buffer_id());
            state.set_status("Switched to " + buffer_display_name(state.active_buffer()));
            return true;
        case EditorAction::PreviousBuffer:
            state.session.switch_to_id(state.active_window().buffer_id);
            state.session.previous_buffer();
            state.show_buffer_in_active_window(state.session.active_buffer_id());
            state.set_status("Switched to " + buffer_display_name(state.active_buffer()));
            return true;
        case EditorAction::SplitHorizontal:
            state.set_status(state.split_active_window(WindowSplitDirection::Horizontal) ? "Split window" : "Could not split window");
            return true;
        case EditorAction::SplitVertical:
            state.set_status(state.split_active_window(WindowSplitDirection::Vertical) ? "Split window" : "Could not split window");
            return true;
        case EditorAction::CloseWindow:
            if (!state.close_active_window() && !state.should_quit) {
                state.set_status("Could not close window");
            }
            return true;
        case EditorAction::CloseOtherWindows:
            state.set_status(state.close_other_windows() ? "Closed other windows" : "No other windows");
            return true;
        case EditorAction::FocusWindowLeft:
            state.set_status(focus_window_direction(state, WindowMoveDirection::Left) ? "Window left" : "No window left");
            return true;
        case EditorAction::FocusWindowRight:
            state.set_status(focus_window_direction(state, WindowMoveDirection::Right) ? "Window right" : "No window right");
            return true;
        case EditorAction::FocusWindowUp:
            state.set_status(focus_window_direction(state, WindowMoveDirection::Up) ? "Window up" : "No window up");
            return true;
        case EditorAction::FocusWindowDown:
            state.set_status(focus_window_direction(state, WindowMoveDirection::Down) ? "Window down" : "No window down");
            return true;
        case EditorAction::Suspend:
            log_debug("Suspend action triggered - calling suspend_editor");
            suspend_editor(state);
            return true;
        default:
            return false;
    }
}

bool handle_prompt_action(EditorState &state, EditorAction action, wint_t key) {
    EditorState::BufferUiState &buffer_state = state.active_buffer_cache();
    switch (action) {
        case EditorAction::CommandExecute:
            state.execute_command();
            return true;
        case EditorAction::CommandBackspace:
            if (state.prompt_cursor > 0) {
                state.command_buffer.erase(state.prompt_cursor - 1, 1);
                --state.prompt_cursor;
            }
            return true;
        case EditorAction::PromptMoveLeft:
            if (state.prompt_cursor > 0) {
                --state.prompt_cursor;
            }
            return true;
        case EditorAction::PromptMoveRight:
            move_prompt_cursor(state, state.prompt_cursor + 1);
            return true;
        case EditorAction::PromptMoveStart:
            state.prompt_cursor = 0;
            return true;
        case EditorAction::PromptMoveEnd:
            move_prompt_cursor(state, std::numeric_limits<std::size_t>::max());
            return true;
        case EditorAction::CommandHistoryPrevious:
            state.browse_prompt_history(true);
            return true;
        case EditorAction::CommandHistoryNext:
            state.browse_prompt_history(false);
            return true;
        case EditorAction::ShowCommandCompletion:
            state.show_command_completion();
            return true;
        case EditorAction::CommandInsert:
            state.command_buffer.insert(
                state.command_buffer.begin() + static_cast<std::ptrdiff_t>(state.prompt_cursor),
                static_cast<char32_t>(key));
            ++state.prompt_cursor;
            return true;
        case EditorAction::SearchExecute:
            state.add_prompt_history_entry(state.search_buffer);
            buffer_state.active_search_pattern = state.search_buffer;
            state.refresh_search_matches(true);
            if (!buffer_state.search_pattern_valid) {
                state.set_search_status("invalid regex");
                return true;
            }
            state.mode = Mode::Normal;
            state.set_status(buffer_state.search_matches.empty() ? "No search matches" : "Search applied");
            return true;
        case EditorAction::SearchBackspace:
            if (state.prompt_cursor > 0) {
                state.search_buffer.erase(state.prompt_cursor - 1, 1);
                --state.prompt_cursor;
            }
            buffer_state.active_search_pattern = state.search_buffer;
            state.refresh_search_matches(true);
            state.set_search_status(buffer_state.search_pattern_valid ? "" : "invalid regex");
            return true;
        case EditorAction::SearchInsert:
            state.search_buffer.insert(
                state.search_buffer.begin() + static_cast<std::ptrdiff_t>(state.prompt_cursor),
                static_cast<char32_t>(key));
            ++state.prompt_cursor;
            buffer_state.active_search_pattern = state.search_buffer;
            state.refresh_search_matches(true);
            state.set_search_status(buffer_state.search_pattern_valid ? "" : "invalid regex");
            return true;
        case EditorAction::SearchHistoryPrevious:
            state.browse_prompt_history(true);
            buffer_state.active_search_pattern = state.search_buffer;
            state.refresh_search_matches(true);
            state.set_search_status(buffer_state.search_pattern_valid ? "" : "invalid regex");
            return true;
        case EditorAction::SearchHistoryNext:
            state.browse_prompt_history(false);
            buffer_state.active_search_pattern = state.search_buffer;
            state.refresh_search_matches(true);
            state.set_search_status(buffer_state.search_pattern_valid ? "" : "invalid regex");
            return true;
        default:
            return false;
    }
}

bool handle_runtime_action(EditorState &state, EditorAction action) {
    switch (action) {
        case EditorAction::GoToDefinition:
            state.request_definition();
            return true;
        case EditorAction::ShowHover:
            state.request_hover();
            return true;
        case EditorAction::ShowCompletion:
            state.request_completion();
            return true;
        case EditorAction::ShowKeyHints:
            if (popup_is_key_hints(state) && state.popup.sticky) {
                state.dismiss_popup();
                state.set_status(mode_name(state.mode));
            } else {
                refresh_key_hint_popup(state, true);
                state.set_status("Key hints");
            }
            return true;
        case EditorAction::SelectEnclosingAst:
            state.select_enclosing_ast();
            return true;
        case EditorAction::SelectInnerAst:
            state.select_inner_ast();
            return true;
        case EditorAction::GoToFileUnderCursor: {
            std::string error_message;
            if (state.lua.execute_command(state, "open-file-under-cursor", std::string(), error_message)) {
                return true;
            }
            state.set_status(error_message);
            return true;
        }
        case EditorAction::JumpBack:
            navigate_jump_history(state, state.jump_stack.back, state.jump_stack.forward, "No older jump", "Jumped back");
            return true;
        case EditorAction::JumpForward:
            navigate_jump_history(state, state.jump_stack.forward, state.jump_stack.back, "No newer jump", "Jumped forward");
            return true;
        case EditorAction::ToggleDiagnosticsVisibility:
            state.diagnostics_visible = !state.diagnostics_visible;
            state.set_status(state.diagnostics_visible ? "Diagnostics shown" : "Diagnostics hidden");
            return true;
        case EditorAction::ToggleDiagnosticsPanel:
            state.show_diagnostics_summary();
            return true;
        default:
            return false;
    }
}

void execute_action(EditorState &state, EditorAction action, wint_t key) {
    log_debug(
        "action name=" + action_name(action) +
        " key=" + std::to_string(static_cast<long long>(key)) +
        " mode=" + mode_name(state.mode));
    if (!handle_motion_action(state, action) &&
        !handle_mode_and_insert_action(state, action, key) &&
        !handle_edit_action(state, action) &&
        !handle_window_action(state, action) &&
        !handle_prompt_action(state, action, key) &&
        !handle_runtime_action(state, action)) {
        return;
    }
    if (!state.should_quit && state.windows.active_window()) {
        state.sync_window_view_from_core(state.windows.active_window_id());
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
        if (action_handles_own_repeat(*dispatch.action)) {
            execute_action(state, *dispatch.action, key);
            return;
        }
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
    if (state.recording.group_depth == 0) {
        return;
    }
    if (state.recording.group_depth > 1) {
        state.recording.record_group_input( ")", ')', false);
        --state.recording.group_depth;
        return;
    }

    std::vector<EditorState::RecordedInput> inputs = state.recording.group_inputs;
    std::size_t repeat = state.recording.group_repeat_count;
    log_debug(
        "command group close repeat=" + std::to_string(repeat) +
        " inputs=[" + join_logged_tokens(logged_tokens(inputs)) + "]");

    if (state.recording.recording && state.recording.replay_depth == 0) {
        auto open = std::find_if(
            state.recording.inputs.rbegin(),
            state.recording.inputs.rend(),
            [](const EditorState::RecordedInput &input) { return input.token == "("; });
        if (open != state.recording.inputs.rend()) {
            std::size_t replace_index = static_cast<std::size_t>(std::distance(open, state.recording.inputs.rend()) - 1);
            state.recording.inputs.resize(replace_index);
            state.recording.inputs.push_back({"(", '(', false});
            state.recording.inputs.insert(state.recording.inputs.end(), inputs.begin(), inputs.end());
            state.recording.inputs.push_back({")", ')', false});
        }
    }

    state.recording.group_inputs.clear();
    state.recording.group_depth = 0;
    state.recording.group_repeat_count = 1;
    state.recording.replay_group_inputs(state, inputs, repeat);
    state.active_core().end_compound_edit();
    state.set_status(mode_name(state.mode));
}

void handle_group_open(EditorState &state) {
    if (state.recording.group_depth == 0) {
        state.recording.group_repeat_count = take_repeat_count(state);
        state.recording.group_inputs.clear();
        state.active_core().begin_compound_edit();
        log_debug("command group open repeat=" + std::to_string(state.recording.group_repeat_count));
    } else {
        state.recording.record_group_input( "(", '(', false);
        log_debug("command group nested open depth=" + std::to_string(state.recording.group_depth + 1));
    }
    ++state.recording.group_depth;
    state.set_status("(");
}

void process_input_token(EditorState &state, const std::string &token, wint_t key, bool printable) {
    if (state.recording.can_start() && mode_supports_command_language(state)) {
        state.recording.begin(state);
    }
    if (mode_supports_command_language(state) && state.pending.motion != PendingMotion::None && printable) {
        state.recording.record_group_input( token, key, printable);
        state.recording.record_command_input( token, key, printable);
        execute_pending_motion(state, static_cast<char32_t>(key));
        state.recording.finalize(state);
        return;
    }
    if (state.mode == Mode::Normal && state.pending.replace_count > 0 && token == "esc") {
        state.pending.replace_count = 0;
        state.set_status(mode_name(state.mode));
        state.recording.finalize(state);
        return;
    }
    if (state.mode == Mode::Normal && state.pending.replace_count > 0 && printable) {
        state.recording.record_group_input( token, key, printable);
        state.recording.record_command_input( token, key, printable);
        EditorCore &core = state.active_core();
        Position cursor = core.cursor();
        std::size_t line_length = core.line_length(cursor.row);
        if (cursor.column >= line_length) {
            state.pending.replace_count = 0;
            state.set_status("No character to replace");
            state.recording.finalize(state);
            return;
        }
        std::size_t replace_count = std::min(state.pending.replace_count, line_length - cursor.column);
        core.replace_range(
            {{cursor.row, cursor.column}, {cursor.row, cursor.column + replace_count}},
            std::u32string(replace_count, static_cast<char32_t>(key)));
        core.set_cursor(cursor);
        state.pending.replace_count = 0;
        state.set_status("Replaced character");
        state.recording.finalize(state);
        return;
    }

    if (mode_supports_command_language(state) && state.pending.motion == PendingMotion::None && state.pending.tokens.empty()) {
        if (token == "(") {
            state.recording.record_command_input( token, key, printable);
            handle_group_open(state);
            state.recording.finalize(state);
            return;
        }
        if (token == ")" && state.recording.group_depth > 0) {
            state.recording.record_command_input( token, key, printable);
            handle_group_close(state);
            state.recording.finalize(state);
            return;
        }
        if (token_starts_repeat(state, token)) {
            state.recording.record_group_input( token, key, printable);
            state.recording.record_command_input( token, key, printable);
            state.pending.repeat_digits += token;
            state.set_status(state.pending.repeat_digits);
            return;
        }
        if (token == "%" && (state.mode == Mode::Normal || state.mode == Mode::Visual || state.mode == Mode::VisualLine)) {
            jump_to_matching_pair(state, state.mode != Mode::Normal);
            state.recording.finalize(state);
            return;
        }
    }

    state.recording.record_group_input( token, key, printable);
    state.recording.record_command_input( token, key, printable);

    KeyDispatch dispatch = dispatch_key_sequence(state.keybindings, mode_key(state), state.pending.tokens, token, printable);
    if (dispatch.waiting_for_more) {
        refresh_key_hint_popup(state, popup_is_key_hints(state) && state.popup.sticky);
        state.set_status(u32_to_utf8(utf8_to_u32(token)));
        return;
    }
    if (popup_is_key_hints(state) && !state.popup.sticky) {
        state.dismiss_popup();
    }
    execute_dispatch(state, dispatch, key);
    sync_key_hint_popup(state);
    state.recording.finalize(state);
}

void handle_keymap_input(EditorState &state, wint_t key, bool is_special) {
    std::optional<std::string> token = key_token(key, is_special);
    if (!token) {
        return;
    }
    log_input_token(state, *token, key, is_printable_input(key, is_special), is_special);
    if (state.handle_popup_input(*token)) {
        return;
    }
    if (state.popup.visible && state.popup.kind != PopupKind::KeyHints) {
        state.dismiss_popup();
    }
    bool printable = is_printable_input(key, is_special);
    if (handle_prompt_token(state, *token, key, printable)) {
        return;
    }
    process_input_token(state, *token, key, printable);
}

void handle_mouse_input(EditorState &state) {
    state.dismiss_popup();
    MEVENT event;
    if (getmouse(&event) != OK) {
        return;
    }
    mmask_t accepted = BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED;
    if ((event.bstate & accepted) == 0) {
        return;
    }

    std::optional<ClickedBufferPosition> clicked = buffer_position_from_screen_point(state, event.y, event.x);
    if (!clicked) {
        return;
    }

    state.pending.tokens.clear();
    state.pending.motion = PendingMotion::None;
    state.pending.motion_repeat_count = 1;
    state.pending.replace_count = 0;
    state.pending.repeat_digits.clear();
    state.focus_window(clicked->window_id);
    state.active_core().set_cursor(clicked->position);
    state.sync_window_view_from_core(state.windows.active_window_id());
    if (state.mode == Mode::Command) {
        state.set_status(":");
        return;
    }
    if (state.mode == Mode::Search) {
        state.set_search_status(state.active_buffer_cache().search_pattern_valid ? "" : "invalid regex");
        return;
    }
    state.set_status(mode_name(state.mode));
}

void EditorState::handle_input() {
    wint_t key = 0;
    int result = get_wch(&key);
    bool is_special = result == KEY_CODE_YES;
    if (result == ERR) {
        return;
    }

    if (!is_special && key == 27) {
        wint_t next_key = 0;
        std::vector<wint_t> escape_keys;
        timeout(0);
        int next_result = get_wch(&next_key);
        std::optional<int> timeout_ms = runtime.idle_wait_timeout_ms();
        timeout(timeout_ms.has_value() ? *timeout_ms : -1);
        if (next_result != ERR) {
            bool next_special = next_result == KEY_CODE_YES;
            std::optional<std::string> alt_token;
            if (next_special && next_key == KEY_UP) {
                alt_token = "alt-up";
            } else if (next_special && next_key == KEY_DOWN) {
                alt_token = "alt-down";
            }
            if (alt_token) {
                process_input_token(*this, *alt_token, next_key, false);
                return;
            }
            escape_keys.push_back(next_key);
            if (!next_special) {
                timeout(0);
                while (escape_keys.size() < 5) {
                    wint_t extra_key = 0;
                    int extra_result = get_wch(&extra_key);
                    if (extra_result == ERR || extra_result == KEY_CODE_YES) {
                        if (extra_result == KEY_CODE_YES) {
                            escape_keys.push_back(extra_key);
                        }
                        break;
                    }
                    escape_keys.push_back(extra_key);
                }
                timeout(timeout_ms.has_value() ? *timeout_ms : -1);
                if (std::optional<std::string> shift_token = shift_arrow_token_from_escape(escape_keys)) {
                    process_input_token(*this, *shift_token, 0, false);
                    return;
                }
                if (std::optional<std::string> shift_token = shift_home_end_token_from_escape(escape_keys)) {
                    process_input_token(*this, *shift_token, 0, false);
                    return;
                }
            }
            push_back_keys(escape_keys);
        }
    }

    if (is_special && key == KEY_MOUSE) {
        handle_mouse_input(*this);
        return;
    }

    if (motion_is_character_based(pending.motion)) {
        if (!is_special && key == 27) {
            pending.motion = PendingMotion::None;
            pending.motion_repeat_count = 1;
            pending.repeat_digits.clear();
            set_status(mode_name(mode));
            return;
        }
        if (!is_special) {
            std::optional<std::string> token = key_token(key, false);
            if (token) {
                recording.record_group_input( *token, key, true);
            }
            execute_pending_motion(*this, static_cast<char32_t>(key));
            sync_window_view_from_core(windows.active_window_id());
        }
        return;
    }

    handle_keymap_input(*this, key, is_special);
}

void update_input_timeout(const EditorState &state) {
    std::optional<int> timeout_ms = state.runtime.idle_wait_timeout_ms();
    if (std::optional<int> lua_timeout = state.lua.idle_wait_timeout_ms()) {
        if (!timeout_ms || *lua_timeout < *timeout_ms) {
            timeout_ms = lua_timeout;
        }
    }
    timeout(timeout_ms.has_value() ? *timeout_ms : -1);
}

void EditorState::handle_service_events() {
    for (const ServiceEvent &event : runtime.take_service_events()) {
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

            EditorState::JumpLocation origin = current_jump_location(*this);
            EditorState::JumpLocation target = {*command.document_uri, file_path_from_uri(*command.document_uri), *command.position};
            if (same_jump_location(origin, target)) {
                set_status("Already at definition");
                continue;
            }

            EditorBuffer *buffer = session.find_buffer_by_uri(*command.document_uri);
            if (!buffer) {
                std::string path = file_path_from_uri(*command.document_uri);
                if (path.empty()) {
                    set_status("Definition target unavailable");
                    continue;
                }
                buffer = session.open_file(path, true);
                if (!buffer) {
                    set_status("Could not open definition target");
                    continue;
                }
                show_buffer_in_active_window(buffer->id);
            } else {
                if (std::optional<std::size_t> window_id = windows.find_window_showing_buffer(buffer->id)) {
                    focus_window(*window_id);
                } else {
                    show_buffer_in_active_window(buffer->id);
                }
            }

            buffer->core.set_cursor(*command.position);
            if (std::optional<std::size_t> window_id = windows.find_window_showing_buffer(buffer->id)) {
                sync_window_view_from_core(*window_id);
            }
            if (jump_stack.back.empty() || !same_jump_location(jump_stack.back.back(), origin)) {
                jump_stack.back.push_back(std::move(origin));
            }
            jump_stack.forward.clear();
            set_status("Opened definition");
            continue;
        }
        if (command.type == EditorCommandType::ShowPopup) {
            if (command.popup_kind == PopupKind::Menu) {
                if (!command.document_uri || *command.document_uri != active_core().document_uri()) {
                    log_debug("completion popup dropped: uri mismatch");
                    continue;
                }
                if (event.document_version != 0 && event.document_version != active_core().document_version()) {
                    log_debug(
                        "completion popup dropped: version mismatch event=" + std::to_string(event.document_version) +
                        " current=" + std::to_string(active_core().document_version()));
                    continue;
                }
                log_debug("completion popup shown count=" + std::to_string(command.popup_items.size()));
                show_menu_popup(*this, command.title, command.popup_items);
            } else {
                show_popup(command.title, utf8_to_u32(command.message));
            }
            continue;
        }
        if (command.type == EditorCommandType::ClearPopup) {
            dismiss_popup();
            continue;
        }
        if (command.type == EditorCommandType::SetSelectionRange) {
            if (!command.document_uri || !command.selection_range) {
                continue;
            }
            EditorBuffer *buffer = session.find_buffer_by_uri(*command.document_uri);
            if (!buffer) {
                continue;
            }
            std::optional<std::size_t> window_id = windows.find_window_showing_buffer(buffer->id);
            if (window_id) {
                focus_window(*window_id);
            } else {
                show_buffer_in_active_window(buffer->id);
                window_id = windows.active_window_id();
            }
        }
        EditorCore *target = &active_core();
        if (command.document_uri) {
            EditorBuffer *buffer = session.find_buffer_by_uri(*command.document_uri);
            if (!buffer) {
                continue;
            }
            if (command.type == EditorCommandType::MoveCursor || command.type == EditorCommandType::SetSelectionRange) {
                if (std::optional<std::size_t> window_id = windows.find_window_showing_buffer(buffer->id)) {
                    focus_window(*window_id);
                } else {
                    show_buffer_in_active_window(buffer->id);
                }
            }
            target = &buffer->core;
        }
        EditorCommandResult result = apply_editor_command(*target, command);
        if (command.type == EditorCommandType::MoveCursor || command.type == EditorCommandType::SetSelectionRange) {
            if (command.document_uri) {
                if (EditorBuffer *buffer = session.find_buffer_by_uri(*command.document_uri)) {
                    if (std::optional<std::size_t> window_id = windows.find_window_showing_buffer(buffer->id)) {
                        sync_window_view_from_core(*window_id);
                        if (command.type == EditorCommandType::SetSelectionRange) {
                            EditorState::WindowUiState &ui = window_ui(*window_id);
                            ui.ast_selection_document_uri = *command.document_uri;
                            ui.ast_selection_document_version = event.document_version;
                            ui.ast_selection_cursor = command.position.value_or(buffer->core.cursor());
                            ui.ast_selection_ranges = command.selection_ranges;
                            ui.ast_selection_index = 0;
                        }
                    }
                }
            } else if (windows.active_window()) {
                sync_window_view_from_core(windows.active_window_id());
            }
            if (command.type == EditorCommandType::SetSelectionRange) {
                mode = Mode::Visual;
                set_status("Selected AST node");
            }
        }
        if (result.status_message) {
            set_status(*result.status_message);
        }
        (void)result;
    }
}

// PendingInputState methods

std::size_t EditorState::PendingInputState::current_repeat_count() const {
    if (repeat_digits.empty()) {
        return 1;
    }
    std::size_t repeat = 0;
    for (char ch : repeat_digits) {
        repeat = repeat * 10 + static_cast<std::size_t>(ch - '0');
    }
    return repeat == 0 ? 1 : repeat;
}

std::size_t EditorState::PendingInputState::take_repeat_count() {
    std::size_t repeat = current_repeat_count();
    repeat_digits.clear();
    return repeat;
}

bool EditorState::PendingInputState::token_starts_repeat(const std::string &token) const {
    if (!token_is_digit(token)) {
        return false;
    }
    if (!repeat_digits.empty()) {
        return true;
    }
    return token != "0";
}

void EditorState::PendingInputState::clear() {
    tokens.clear();
    motion = PendingMotion::None;
    repeat_digits.clear();
    motion_repeat_count = 1;
    replace_count = 0;
}
