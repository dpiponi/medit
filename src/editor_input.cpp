#include "editor_internal.hpp"

#include "logger.hpp"

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

bool split_active_window(EditorState &state, WindowSplitDirection direction) {
    if (!state.windows.split_active(direction)) {
        return false;
    }
    sync_active_window_buffer(state);
    active_buffer_ui(state);
    sync_window_view_from_core(state, state.windows.active_window_id());
    return true;
}

bool close_active_window(EditorState &state) {
    if (state.windows.window_count() <= 1) {
        if (!can_quit_without_force(state)) {
            set_status(state, "Unsaved changes; use :q! to quit");
            return false;
        }
        quit_editor(state);
        return true;
    }

    std::size_t closing_window_id = state.windows.active_window_id();
    if (!state.windows.close_active()) {
        return false;
    }
    state.window_ui.erase(closing_window_id);
    sync_active_window_buffer(state);
    active_buffer_ui(state);
    return true;
}

bool close_other_windows(EditorState &state) {
    if (!state.windows.close_others()) {
        return false;
    }
    for (auto it = state.window_ui.begin(); it != state.window_ui.end();) {
        if (!state.windows.find_window(it->first)) {
            it = state.window_ui.erase(it);
        } else {
            ++it;
        }
    }
    sync_active_window_buffer(state);
    active_buffer_ui(state);
    return true;
}

bool focus_window_direction(EditorState &state, WindowMoveDirection direction) {
    int screen_rows = 0;
    int screen_cols = 0;
    getmaxyx(stdscr, screen_rows, screen_cols);
    if (!state.windows.focus_direction(direction, screen_rows, screen_cols, 2)) {
        return false;
    }
    sync_active_window_buffer(state);
    active_buffer_ui(state);
    sync_core_view_from_window(state, state.windows.active_window_id());
    return true;
}

void page_up(EditorState &state) {
    active_core(state).move_by_lines(-page_step());
    sync_window_view_from_core(state, state.windows.active_window_id());
}

void page_down(EditorState &state) {
    active_core(state).move_by_lines(page_step());
    sync_window_view_from_core(state, state.windows.active_window_id());
}

void half_page_up(EditorState &state) {
    active_core(state).move_by_lines(-half_page_step());
    sync_window_view_from_core(state, state.windows.active_window_id());
}

void half_page_down(EditorState &state) {
    active_core(state).move_by_lines(half_page_step());
    sync_window_view_from_core(state, state.windows.active_window_id());
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

std::size_t take_repeat_count(EditorState &state);

void prepare_visual_motion(EditorState &state) {
    if (state.mode != Mode::Normal) {
        return;
    }
    state.mode = Mode::Visual;
    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    active_core(state).begin_selection(SelectionMode::Character);
    set_status(state, mode_name(state.mode));
}

void begin_pending_motion(EditorState &state, PendingMotion motion, const char *status) {
    state.pending_motion_repeat_count = take_repeat_count(state);
    state.pending_motion = motion;
    set_status(state, status);
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

bool move_cursor_to_selection_boundary(EditorState &state, bool to_end) {
    EditorCore &core = active_core(state);
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
    EditorCore &core = active_core(state);
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
    if (state.pending_tokens.empty()) {
        return mode_name(state.mode) + " keys";
    }
    return mode_name(state.mode) + " after " + joined_key_sequence(state.pending_tokens);
}

std::vector<PopupMenuItem> key_hint_popup_items(const EditorState &state) {
    std::vector<KeyHint> hints = key_hints_for_prefix(state.keybindings, mode_key(state), state.pending_tokens);
    std::vector<PopupMenuItem> items;
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
    std::vector<PopupMenuItem> items = key_hint_popup_items(state);
    if (items.empty()) {
        dismiss_popup(state);
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

std::vector<std::pair<std::size_t, std::size_t>> capture_buffer_versions(const EditorState &state) {
    std::vector<std::pair<std::size_t, std::size_t>> versions;
    versions.reserve(state.session.buffers().size());
    for (const EditorBuffer &buffer : state.session.buffers()) {
        versions.push_back({buffer.id, buffer.core.document_version()});
    }
    return versions;
}

bool command_recording_can_start(const EditorState &state) {
    return state.replay_depth == 0 && !state.command_recording;
}

void begin_command_recording(EditorState &state) {
    state.command_recording = true;
    state.command_recording_nonrepeatable = false;
    state.command_inputs.clear();
    state.command_buffer_versions = capture_buffer_versions(state);
}

void reset_command_recording(EditorState &state) {
    state.command_recording = false;
    state.command_recording_nonrepeatable = false;
    state.command_inputs.clear();
    state.command_buffer_versions.clear();
}

bool command_recording_complete(const EditorState &state) {
    if (!state.command_recording) {
        return false;
    }
    if (state.replay_depth > 0) {
        return false;
    }
    if (state.group_depth != 0 || !state.pending_tokens.empty() || state.pending_motion != PendingMotion::None ||
        state.pending_replace_count != 0) {
        return false;
    }
    return state.mode != Mode::Insert && state.mode != Mode::Command && state.mode != Mode::Search;
}

bool command_recording_changed_buffer(const EditorState &state) {
    std::vector<std::pair<std::size_t, std::size_t>> after = capture_buffer_versions(state);
    return after != state.command_buffer_versions;
}

void finalize_command_recording(EditorState &state) {
    if (!command_recording_complete(state)) {
        return;
    }
    bool changed = command_recording_changed_buffer(state);
    if (!state.command_recording_nonrepeatable && changed &&
        !state.command_inputs.empty()) {
        state.last_repeatable_command = state.command_inputs;
    }
    reset_command_recording(state);
}

void record_group_input(EditorState &state, const std::string &token, wint_t key, bool printable) {
    if (state.group_depth == 0) {
        return;
    }
    state.group_inputs.push_back({token, key, printable});
}

void record_command_input(EditorState &state, const std::string &token, wint_t key, bool printable) {
    if (!state.command_recording || state.replay_depth > 0) {
        return;
    }
    state.command_inputs.push_back({token, key, printable});
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

bool repeat_last_command(EditorState &state) {
    static constexpr std::size_t kMaxReplayDepth = 16;
    if (state.last_repeatable_command.empty()) {
        set_status(state, "Nothing to repeat");
        return false;
    }
    if (state.replay_depth >= kMaxReplayDepth) {
        set_status(state, "Repeat nesting too deep");
        return false;
    }

    std::size_t repeat = take_repeat_count(state);
    active_core(state).begin_compound_edit();
    ++state.replay_depth;
    for (std::size_t iteration = 0; iteration < repeat; ++iteration) {
        for (const EditorState::RecordedInput &input : state.last_repeatable_command) {
            process_input_token(state, input.token, input.key, input.printable);
        }
    }
    --state.replay_depth;
    active_core(state).end_compound_edit();
    set_status(state, "Repeated command");
    return true;
}

void execute_action(EditorState &state, EditorAction action, wint_t key) {
    EditorCore &core = active_core(state);
    EditorState::BufferUiState &buffer_state = active_buffer_cache(state);
    switch (action) {
        case EditorAction::MoveLeft:
            core.move_left();
            break;
        case EditorAction::VisualMoveLeft:
            prepare_visual_motion(state);
            core.move_left();
            break;
        case EditorAction::MoveRight:
            core.move_right();
            break;
        case EditorAction::VisualMoveRight:
            prepare_visual_motion(state);
            core.move_right();
            break;
        case EditorAction::MoveUp:
            core.move_up();
            break;
        case EditorAction::VisualMoveUp:
            prepare_visual_motion(state);
            core.move_up();
            break;
        case EditorAction::MoveDown:
            core.move_down();
            break;
        case EditorAction::VisualMoveDown:
            prepare_visual_motion(state);
            core.move_down();
            break;
        case EditorAction::MoveLineStart:
            core.move_line_start();
            break;
        case EditorAction::VisualMoveLineStart:
            prepare_visual_motion(state);
            core.move_line_start();
            break;
        case EditorAction::MoveLineEnd:
            core.move_line_end();
            break;
        case EditorAction::VisualMoveLineEnd:
            prepare_visual_motion(state);
            core.move_line_end();
            break;
        case EditorAction::FindForward:
            begin_pending_motion(state, PendingMotion::FindForward, "f");
            break;
        case EditorAction::VisualFindForward:
            prepare_visual_motion(state);
            begin_pending_motion(state, PendingMotion::FindForward, "F");
            break;
        case EditorAction::FindBackward:
            begin_pending_motion(state, PendingMotion::FindBackward, "gf");
            break;
        case EditorAction::VisualFindBackward:
            prepare_visual_motion(state);
            begin_pending_motion(state, PendingMotion::FindBackward, "gF");
            break;
        case EditorAction::TillForward:
            begin_pending_motion(state, PendingMotion::TillForward, "t");
            break;
        case EditorAction::VisualTillForward:
            prepare_visual_motion(state);
            begin_pending_motion(state, PendingMotion::TillForward, "T");
            break;
        case EditorAction::TillBackward:
            begin_pending_motion(state, PendingMotion::TillBackward, "gt");
            break;
        case EditorAction::VisualTillBackward:
            prepare_visual_motion(state);
            begin_pending_motion(state, PendingMotion::TillBackward, "gT");
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
            begin_insert_session(state);
            if (effective_autoindent(state.config, core.file_path())) {
                core.open_line_below_with_autoindent();
            } else {
                core.open_line_below();
            }
            state.mode = Mode::Insert;
            set_status(state, mode_name(state.mode));
            break;
        case EditorAction::OpenLineAbove:
            begin_insert_session(state);
            core.open_line_above();
            state.mode = Mode::Insert;
            set_status(state, mode_name(state.mode));
            break;
        case EditorAction::DeleteChar:
            core.delete_character_under_cursor();
            set_status(state, "Deleted character");
            break;
        case EditorAction::ReplaceChar:
            state.pending_replace_count = take_repeat_count(state);
            set_status(state, "r");
            break;
        case EditorAction::RepeatLastCommand:
            state.command_recording_nonrepeatable = true;
            repeat_last_command(state);
            break;
        case EditorAction::Undo:
            state.command_recording_nonrepeatable = true;
            set_status(state, core.undo() ? "Undid change" : "Nothing to undo");
            break;
        case EditorAction::Redo:
            state.command_recording_nonrepeatable = true;
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
        case EditorAction::VisualGotoTop:
            prepare_visual_motion(state);
            core.move_to_first_line();
            core.move_line_start();
            set_status(state, "Top of file");
            break;
        case EditorAction::GotoBottom:
            core.move_to_last_line();
            core.move_line_start();
            set_status(state, "Bottom of file");
            break;
        case EditorAction::VisualGotoBottom:
            prepare_visual_motion(state);
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
            state.session.switch_to_id(active_window(state).buffer_id);
            state.session.next_buffer();
            show_buffer_in_active_window(state, state.session.active_buffer_id());
            set_status(state, "Switched to " + active_core(state).display_file_name());
            break;
        case EditorAction::PreviousBuffer:
            state.session.switch_to_id(active_window(state).buffer_id);
            state.session.previous_buffer();
            show_buffer_in_active_window(state, state.session.active_buffer_id());
            set_status(state, "Switched to " + active_core(state).display_file_name());
            break;
        case EditorAction::SplitHorizontal:
            set_status(state, split_active_window(state, WindowSplitDirection::Horizontal) ? "Split window" : "Could not split window");
            break;
        case EditorAction::SplitVertical:
            set_status(state, split_active_window(state, WindowSplitDirection::Vertical) ? "Split window" : "Could not split window");
            break;
        case EditorAction::CloseWindow:
            if (!close_active_window(state) && !state.should_quit) {
                set_status(state, "Could not close window");
            }
            break;
        case EditorAction::CloseOtherWindows:
            set_status(state, close_other_windows(state) ? "Closed other windows" : "No other windows");
            break;
        case EditorAction::FocusWindowLeft:
            set_status(state, focus_window_direction(state, WindowMoveDirection::Left) ? "Window left" : "No window left");
            break;
        case EditorAction::FocusWindowRight:
            set_status(state, focus_window_direction(state, WindowMoveDirection::Right) ? "Window right" : "No window right");
            break;
        case EditorAction::FocusWindowUp:
            set_status(state, focus_window_direction(state, WindowMoveDirection::Up) ? "Window up" : "No window up");
            break;
        case EditorAction::FocusWindowDown:
            set_status(state, focus_window_direction(state, WindowMoveDirection::Down) ? "Window down" : "No window down");
            break;
        case EditorAction::Suspend:
            suspend_editor(state);
            break;
        case EditorAction::EnterNormalMode:
            enter_normal_mode(state);
            break;
        case EditorAction::InsertNewline:
            begin_insert_session(state);
            if (effective_autoindent(state.config, core.file_path())) {
                core.insert_newline_with_autoindent();
            } else {
                core.insert_newline();
            }
            break;
        case EditorAction::InsertSoftTab:
            begin_insert_session(state);
            core.insert_soft_tab(
                effective_tabstop(state.config, core.file_path()),
                effective_softtabstop(state.config, core.file_path()),
                effective_expandtab(state.config, core.file_path()));
            break;
        case EditorAction::InsertOutdent:
            begin_insert_session(state);
            core.outdent_before_cursor(
                effective_tabstop(state.config, core.file_path()),
                effective_softtabstop(state.config, core.file_path()));
            break;
        case EditorAction::Backspace:
            begin_insert_session(state);
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
        case EditorAction::ShowCommandCompletion:
            show_command_completion(state);
            break;
        case EditorAction::SelfInsert:
            begin_insert_session(state);
            core.insert_codepoint(static_cast<char32_t>(key));
            break;
        case EditorAction::CommandInsert:
            state.command_buffer.push_back(static_cast<char32_t>(key));
            break;
        case EditorAction::SearchExecute:
            add_prompt_history_entry(state, state.search_buffer);
            buffer_state.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            if (!buffer_state.search_pattern_valid) {
                set_search_status(state, "invalid regex");
                break;
            }
            state.mode = Mode::Normal;
            set_status(state, buffer_state.search_matches.empty() ? "No search matches" : "Search applied");
            break;
        case EditorAction::SearchBackspace:
            if (!state.search_buffer.empty()) {
                state.search_buffer.pop_back();
            }
            buffer_state.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            set_search_status(state, buffer_state.search_pattern_valid ? "" : "invalid regex");
            break;
        case EditorAction::SearchInsert:
            state.search_buffer.push_back(static_cast<char32_t>(key));
            buffer_state.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            set_search_status(state, buffer_state.search_pattern_valid ? "" : "invalid regex");
            break;
        case EditorAction::SearchHistoryPrevious:
            browse_prompt_history(state, true);
            buffer_state.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            set_search_status(state, buffer_state.search_pattern_valid ? "" : "invalid regex");
            break;
        case EditorAction::SearchHistoryNext:
            browse_prompt_history(state, false);
            buffer_state.active_search_pattern = state.search_buffer;
            refresh_search_matches(state, true);
            set_search_status(state, buffer_state.search_pattern_valid ? "" : "invalid regex");
            break;
        case EditorAction::SearchNext:
            refresh_search_matches(state, false);
            navigate_search_match(state, true);
            break;
        case EditorAction::VisualSearchNext:
            prepare_visual_motion(state);
            refresh_search_matches(state, false);
            navigate_search_match(state, true);
            break;
        case EditorAction::SearchPrevious:
            refresh_search_matches(state, false);
            navigate_search_match(state, false);
            break;
        case EditorAction::VisualSearchPrevious:
            prepare_visual_motion(state);
            refresh_search_matches(state, false);
            navigate_search_match(state, false);
            break;
        case EditorAction::GoToDefinition:
            request_definition(state);
            break;
        case EditorAction::ShowHover:
            request_hover(state);
            break;
        case EditorAction::ShowCompletion:
            request_completion(state);
            break;
        case EditorAction::ShowKeyHints:
            if (popup_is_key_hints(state) && state.popup.sticky) {
                dismiss_popup(state);
                set_status(state, mode_name(state.mode));
            } else {
                refresh_key_hint_popup(state, true);
                set_status(state, "Key hints");
            }
            break;
        case EditorAction::SelectEnclosingAst:
            select_enclosing_ast(state);
            break;
        case EditorAction::SelectInnerAst:
            select_inner_ast(state);
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
        case EditorAction::ToggleDiagnosticsVisibility:
            state.diagnostics_visible = !state.diagnostics_visible;
            set_status(state, state.diagnostics_visible ? "Diagnostics shown" : "Diagnostics hidden");
            break;
        case EditorAction::ToggleDiagnosticsPanel:
            show_diagnostics_summary(state);
            break;
        case EditorAction::DeleteSelection: {
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
        case EditorAction::ChangeSelection: {
            begin_insert_session(state);
            if (core.delete_selection()) {
                state.session.capture_active_clipboard();
                enter_insert_mode(state);
            } else {
                end_insert_session(state);
                set_status(state, "No selection");
            }
            break;
        }
        case EditorAction::ReplaceSelectionWithYank: {
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
        case EditorAction::MoveToSelectionStart:
            if (!move_cursor_to_selection_boundary(state, false)) {
                set_status(state, "No selection");
            }
            break;
        case EditorAction::MoveToSelectionEnd:
            if (!move_cursor_to_selection_boundary(state, true)) {
                set_status(state, "No selection");
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
    if (!state.should_quit && state.windows.active_window()) {
        sync_window_view_from_core(state, state.windows.active_window_id());
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
    if (command_recording_can_start(state) && mode_supports_command_language(state)) {
        begin_command_recording(state);
    }
    if (mode_supports_command_language(state) && state.pending_motion != PendingMotion::None && printable) {
        record_group_input(state, token, key, printable);
        record_command_input(state, token, key, printable);
        execute_pending_motion(state, static_cast<char32_t>(key));
        finalize_command_recording(state);
        return;
    }
    if (state.mode == Mode::Normal && state.pending_replace_count > 0 && token == "esc") {
        state.pending_replace_count = 0;
        set_status(state, mode_name(state.mode));
        finalize_command_recording(state);
        return;
    }
    if (state.mode == Mode::Normal && state.pending_replace_count > 0 && printable) {
        record_group_input(state, token, key, printable);
        record_command_input(state, token, key, printable);
        EditorCore &core = active_core(state);
        Position cursor = core.cursor();
        std::size_t line_length = core.line_length(cursor.row);
        if (cursor.column >= line_length) {
            state.pending_replace_count = 0;
            set_status(state, "No character to replace");
            finalize_command_recording(state);
            return;
        }
        std::size_t replace_count = std::min(state.pending_replace_count, line_length - cursor.column);
        core.replace_range(
            {{cursor.row, cursor.column}, {cursor.row, cursor.column + replace_count}},
            std::u32string(replace_count, static_cast<char32_t>(key)));
        core.set_cursor(cursor);
        state.pending_replace_count = 0;
        set_status(state, "Replaced character");
        finalize_command_recording(state);
        return;
    }

    if (mode_supports_command_language(state) && state.pending_motion == PendingMotion::None && state.pending_tokens.empty()) {
        if (token == "(") {
            record_command_input(state, token, key, printable);
            handle_group_open(state);
            finalize_command_recording(state);
            return;
        }
        if (token == ")" && state.group_depth > 0) {
            record_command_input(state, token, key, printable);
            handle_group_close(state);
            finalize_command_recording(state);
            return;
        }
        if (token_starts_repeat(state, token)) {
            record_group_input(state, token, key, printable);
            record_command_input(state, token, key, printable);
            state.repeat_digits += token;
            set_status(state, state.repeat_digits);
            return;
        }
    }

    record_group_input(state, token, key, printable);
    record_command_input(state, token, key, printable);

    KeyDispatch dispatch = dispatch_key_sequence(state.keybindings, mode_key(state), state.pending_tokens, token, printable);
    if (dispatch.waiting_for_more) {
        refresh_key_hint_popup(state, popup_is_key_hints(state) && state.popup.sticky);
        set_status(state, u32_to_utf8(utf8_to_u32(token)));
        return;
    }
    if (popup_is_key_hints(state) && !state.popup.sticky) {
        dismiss_popup(state);
    }
    execute_dispatch(state, dispatch, key);
    sync_key_hint_popup(state);
    finalize_command_recording(state);
}

void handle_keymap_input(EditorState &state, wint_t key, bool is_special) {
    std::optional<std::string> token = key_token(key, is_special);
    if (!token) {
        return;
    }
    if (handle_popup_input(state, *token)) {
        return;
    }
    if (state.popup.visible && state.popup.kind != PopupKind::KeyHints) {
        dismiss_popup(state);
    }
    process_input_token(state, *token, key, is_printable_input(key, is_special));
}

void handle_mouse_input(EditorState &state) {
    dismiss_popup(state);
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

    state.pending_tokens.clear();
    state.pending_motion = PendingMotion::None;
    state.pending_motion_repeat_count = 1;
    state.pending_replace_count = 0;
    state.repeat_digits.clear();
    focus_window(state, clicked->window_id);
    active_core(state).set_cursor(clicked->position);
    sync_window_view_from_core(state, state.windows.active_window_id());
    if (state.mode == Mode::Command) {
        set_status(state, ":");
        return;
    }
    if (state.mode == Mode::Search) {
        set_search_status(state, active_buffer_cache(state).search_pattern_valid ? "" : "invalid regex");
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

    if (!is_special && key == 27) {
        wint_t next_key = 0;
        std::vector<wint_t> escape_keys;
        timeout(0);
        int next_result = get_wch(&next_key);
        std::optional<int> timeout_ms = state.runtime.idle_wait_timeout_ms();
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
                process_input_token(state, *alt_token, next_key, false);
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
                    process_input_token(state, *shift_token, 0, false);
                    return;
                }
                if (std::optional<std::string> shift_token = shift_home_end_token_from_escape(escape_keys)) {
                    process_input_token(state, *shift_token, 0, false);
                    return;
                }
            }
            push_back_keys(escape_keys);
        }
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
            sync_window_view_from_core(state, state.windows.active_window_id());
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
                show_buffer_in_active_window(state, buffer->id);
            } else {
                if (std::optional<std::size_t> window_id = state.windows.find_window_showing_buffer(buffer->id)) {
                    focus_window(state, *window_id);
                } else {
                    show_buffer_in_active_window(state, buffer->id);
                }
            }

            buffer->core.set_cursor(*command.position);
            if (std::optional<std::size_t> window_id = state.windows.find_window_showing_buffer(buffer->id)) {
                sync_window_view_from_core(state, *window_id);
            }
            if (state.jump_back_stack.empty() || !same_jump_location(state.jump_back_stack.back(), origin)) {
                state.jump_back_stack.push_back(std::move(origin));
            }
            state.jump_forward_stack.clear();
            set_status(state, "Opened definition");
            continue;
        }
        if (command.type == EditorCommandType::ShowPopup) {
            if (command.popup_kind == PopupKind::Menu) {
                if (!command.document_uri || *command.document_uri != active_core(state).document_uri()) {
                    log_debug("completion popup dropped: uri mismatch");
                    continue;
                }
                if (event.document_version != 0 && event.document_version != active_core(state).document_version()) {
                    log_debug(
                        "completion popup dropped: version mismatch event=" + std::to_string(event.document_version) +
                        " current=" + std::to_string(active_core(state).document_version()));
                    continue;
                }
                log_debug("completion popup shown count=" + std::to_string(command.popup_items.size()));
                show_menu_popup(state, command.title, command.popup_items);
            } else {
                show_popup(state, command.title, utf8_to_u32(command.message));
            }
            continue;
        }
        if (command.type == EditorCommandType::ClearPopup) {
            dismiss_popup(state);
            continue;
        }
        if (command.type == EditorCommandType::SetSelectionRange) {
            if (!command.document_uri || !command.selection_range) {
                continue;
            }
            EditorBuffer *buffer = state.session.find_buffer_by_uri(*command.document_uri);
            if (!buffer) {
                continue;
            }
            std::optional<std::size_t> window_id = state.windows.find_window_showing_buffer(buffer->id);
            if (window_id) {
                focus_window(state, *window_id);
            } else {
                show_buffer_in_active_window(state, buffer->id);
                window_id = state.windows.active_window_id();
            }
        }
        EditorCore *target = &active_core(state);
        if (command.document_uri) {
            EditorBuffer *buffer = state.session.find_buffer_by_uri(*command.document_uri);
            if (!buffer) {
                continue;
            }
            if (command.type == EditorCommandType::MoveCursor || command.type == EditorCommandType::SetSelectionRange) {
                if (std::optional<std::size_t> window_id = state.windows.find_window_showing_buffer(buffer->id)) {
                    focus_window(state, *window_id);
                } else {
                    show_buffer_in_active_window(state, buffer->id);
                }
            }
            target = &buffer->core;
        }
        EditorCommandResult result = apply_editor_command(*target, command);
        if (command.type == EditorCommandType::MoveCursor || command.type == EditorCommandType::SetSelectionRange) {
            if (command.document_uri) {
                if (EditorBuffer *buffer = state.session.find_buffer_by_uri(*command.document_uri)) {
                    if (std::optional<std::size_t> window_id = state.windows.find_window_showing_buffer(buffer->id)) {
                        sync_window_view_from_core(state, *window_id);
                        if (command.type == EditorCommandType::SetSelectionRange) {
                            EditorState::WindowUiState &ui = window_ui(state, *window_id);
                            ui.ast_selection_document_uri = *command.document_uri;
                            ui.ast_selection_document_version = event.document_version;
                            ui.ast_selection_cursor = command.position.value_or(buffer->core.cursor());
                            ui.ast_selection_ranges = command.selection_ranges;
                            ui.ast_selection_index = 0;
                        }
                    }
                }
            } else if (state.windows.active_window()) {
                sync_window_view_from_core(state, state.windows.active_window_id());
            }
            if (command.type == EditorCommandType::SetSelectionRange) {
                state.mode = Mode::Visual;
                set_status(state, "Selected AST node");
            }
        }
        if (result.status_message) {
            set_status(state, *result.status_message);
        }
        (void)result;
    }
}
