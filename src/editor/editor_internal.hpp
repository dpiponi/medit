#pragma once

#include "editor_state.hpp"

struct ClickedBufferPosition {
    std::size_t window_id = 0;
    Position position;
};

EditorWindow &active_window(EditorState &state);
const EditorWindow &active_window(const EditorState &state);
EditorBuffer &active_buffer(EditorState &state);
const EditorBuffer &active_buffer(const EditorState &state);
std::string buffer_display_name(const EditorBuffer &buffer);
std::string buffer_text_utf8(const EditorBuffer &buffer);
JsonValue json_position(Position position);
JsonValue json_range(const Range &range);
const char *selection_mode_name(SelectionMode mode);
JsonValue json_buffer_summary(const EditorState &state, const EditorBuffer &buffer);
EditorCore &active_core(EditorState &state);
const EditorCore &active_core(const EditorState &state);
std::wstring u32_to_wstring(const std::u32string &text);
int codepoint_width(char32_t codepoint);
std::size_t codepoint_display_width(char32_t codepoint, std::size_t visual_column, std::size_t tabstop);
std::string mode_name(Mode mode);
std::string prefixed_message(const char *prefix, const std::string &value);
std::string count_label(std::size_t count, const char *singular);
std::string make_status_bar_left_text(
    const EditorState &state,
    const EditorCore &core,
    const std::string &language,
    const std::string &workspace);
std::string make_status_bar_right_text(const EditorState &state, const EditorCore &core, Position cursor);
bool handle_popup_input(EditorState &state, const std::string &token);
void add_prompt_history_entry(EditorState &state, const std::u32string &entry);
void browse_prompt_history(EditorState &state, bool previous);
std::size_t popup_menu_visible_rows_for_screen(int screen_rows);
bool should_render_diagnostics(const EditorState &state, std::size_t window_id);
Lines wrap_annotation_text(const std::u32string &text, int max_cols);
const VisualRows &visual_rows_for_window(const EditorState &state, std::size_t window_id, int buffer_cols);
std::size_t visual_row_for_buffer_row(const VisualRows &rows, std::size_t buffer_row);
void show_diagnostics_summary(EditorState &state);
void navigate_diagnostic(EditorState &state, bool forward);
void execute_command(EditorState &state);
void show_command_completion(EditorState &state);
void navigate_search_match(EditorState &state, bool forward);
void refresh_search_matches_for_window(EditorState &state, std::size_t window_id);
void refresh_search_matches(EditorState &state, bool move_to_best_match);
void restore_shell_terminal_state();
void apply_theme_to_terminal(const Theme &theme);
void setup_terminal(const Theme &theme);
void teardown_terminal();
void suspend_editor(EditorState &state);
void sync_active_window_buffer(EditorState &state);
void show_buffer_in_active_window(EditorState &state, std::size_t buffer_id, bool reset_view = true);
void show_buffer_in_panel(EditorState &state, std::size_t buffer_id, bool focus_panel = false);
void focus_window(EditorState &state, std::size_t window_id);
void begin_insert_session(EditorState &state);
void end_insert_session(EditorState &state);
void enter_normal_mode(EditorState &state);
void enter_insert_mode(EditorState &state);
void enter_command_mode(EditorState &state);
void enter_filter_command_mode(EditorState &state);
void enter_sed_command_mode(EditorState &state);
void enter_search_mode(EditorState &state);
bool can_quit_without_force(const EditorState &state);
void quit_editor(EditorState &state);
std::string shell_single_quote(const std::string &text);
void handle_edit_command(EditorState &state, const std::string &argument);
bool reload_editor_configuration(EditorState &state, std::string &error_message);
void show_lsp_status(EditorState &state);
void show_tree_sitter_status(EditorState &state);
void open_startup_file_picker(EditorState &state, const std::optional<std::filesystem::path> &root = std::nullopt);
void request_definition(EditorState &state);
void request_hover(EditorState &state);
void request_completion(EditorState &state);
void select_enclosing_ast(EditorState &state);
void select_inner_ast(EditorState &state);
EditorState::JumpLocation current_jump_location(const EditorState &state);
bool same_jump_location(const EditorState::JumpLocation &left, const EditorState::JumpLocation &right);
void navigate_jump_history(
    EditorState &state,
    std::vector<EditorState::JumpLocation> &source,
    std::vector<EditorState::JumpLocation> &destination,
    const std::string &empty_status,
    const std::string &success_status);
void set_search_status(EditorState &state, const std::string &suffix = "");
void show_menu_popup(
    EditorState &state,
    std::string title,
    PopupMenuItems items,
    EditorState::PopupApplyTarget apply_target = EditorState::PopupApplyTarget::BufferText,
    std::u32string initial_filter = U"",
    EditorState::PopupFilterMode filter_mode = EditorState::PopupFilterMode::ContainsLabelOrDetail);
void show_key_hints_popup(
    EditorState &state,
    std::string title,
    PopupMenuItems items,
    bool sticky);
bool popup_accepts_input(const EditorState &state);
void dismiss_popup(EditorState &state);
bool split_active_window(EditorState &state, WindowSplitDirection direction);
bool close_active_window(EditorState &state);
bool close_other_windows(EditorState &state);
void handle_input(EditorState &state);
void update_input_timeout(const EditorState &state);
void handle_service_events(EditorState &state);
std::string handle_control_request(EditorState &state, std::string_view request_text);
std::optional<Position> matching_pair_cursor(const EditorCore &core);
std::optional<Position> matching_pair_position(const EditorCore &core, Position pair_position);
bool jump_to_matching_pair(EditorState &state, bool extend_selection);
void run_editor(EditorState &state);
std::optional<std::string> open_startup_files(EditorState &state, int argc, char **argv);
int line_number_width(const EditorCore &core);
std::string build_status_text(const EditorState &state);
std::optional<ClickedBufferPosition> buffer_position_from_screen_point(const EditorState &state, int screen_row, int screen_col);
void draw_editor(const EditorState &state);
void refresh_syntax_highlights(EditorState &state, std::size_t window_id);
void render_frame(EditorState &state);
void initialize_locale();
bool suspend_supported();
void initialize_windows(EditorState &state);
