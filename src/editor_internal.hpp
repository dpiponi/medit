#pragma once

#include "config.hpp"
#include "control_server.hpp"
#include "editor_commands.hpp"
#include "editor_core.hpp"
#include "editor_session.hpp"
#include "editor_windows.hpp"
#include "json.hpp"
#include "keybindings.hpp"
#include "services.hpp"
#include "syntax.hpp"
#include "theme.hpp"

#include <chrono>
#include <curses.h>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
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

enum class CommandPromptKind {
    EditorCommand,
    FilterSelection,
    SedSelection,
};

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

using VisualRows = std::vector<VisualRow>;

struct VisualRowsCache {
    int buffer_cols = -1;
    std::size_t text_revision = std::numeric_limits<std::size_t>::max();
    std::size_t diagnostics_revision = std::numeric_limits<std::size_t>::max();
    std::size_t annotations_revision = std::numeric_limits<std::size_t>::max();
    bool show_diagnostics = true;
    VisualRows rows;
};

struct SubstituteCommand {
    bool whole_buffer = false;
    std::string pattern;
    std::string replacement;
    bool global = false;
};

struct EditorState {
    enum class PopupApplyTarget {
        BufferText,
        CommandBuffer,
    };

    enum class PopupFilterMode {
        ContainsLabelOrDetail,
        PrefixLabelOnly,
    };

    struct PromptHistory {
        Lines entries;
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

    struct WindowUiState {
        std::size_t row_offset = 0;
        std::size_t col_offset = 0;
        EditorViewState view_state;
        std::optional<std::size_t> current_search_match_index;
        Position search_origin;
        std::optional<std::size_t> selected_diagnostic_index;
        std::string ast_selection_document_uri;
        std::size_t ast_selection_document_version = 0;
        Position ast_selection_cursor;
        std::vector<Range> ast_selection_ranges;
        std::size_t ast_selection_index = 0;
    };

    struct BufferUiState {
        std::u32string active_search_pattern;
        std::vector<Range> search_matches;
        bool search_pattern_valid = true;
        std::string compiled_search_pattern_utf8;
        std::unique_ptr<std::regex> compiled_search_regex;
        std::size_t search_matches_version = 0;
        std::map<std::pair<int, bool>, VisualRowsCache> visual_rows_caches;
        std::size_t sorted_diagnostics_revision = std::numeric_limits<std::size_t>::max();
        std::vector<DiagnosticEntryView> sorted_diagnostics;
        std::map<bool, std::pair<std::size_t, std::vector<AnnotationEntryView>>> sorted_annotations;
    };

    struct SyntaxUiState {
        SyntaxSelection syntax_selection;
        std::vector<HighlightSpans> syntax_highlights;
        std::size_t syntax_revision = std::numeric_limits<std::size_t>::max();
        std::size_t pending_syntax_revision = std::numeric_limits<std::size_t>::max();
        std::chrono::steady_clock::time_point syntax_dirty_since{};
        std::optional<std::string> syntax_file_path;
        bool syntax_config_error_reported = false;
    };

    struct PopupState {
        bool visible = false;
        PopupKind kind = PopupKind::Text;
        std::string title;
        std::u32string text;
        PopupMenuItems items;
        std::u32string filter;
        std::vector<std::size_t> filtered_indices;
        std::size_t selected_index = 0;
        std::size_t scroll_offset = 0;
        Mode originating_mode = Mode::Normal;
        PopupApplyTarget apply_target = PopupApplyTarget::BufferText;
        PopupFilterMode filter_mode = PopupFilterMode::ContainsLabelOrDetail;
        bool sticky = false;
    };

    struct PendingInputState {
        std::vector<std::string> tokens;
        PendingMotion motion = PendingMotion::None;
        std::string repeat_digits;
        std::size_t motion_repeat_count = 1;
        std::size_t replace_count = 0;

        std::size_t current_repeat_count() const;
        std::size_t take_repeat_count();
        bool token_starts_repeat(const std::string &token) const;
        void clear();
    };

    struct CommandRecordingState {
        bool recording = false;
        bool recording_nonrepeatable = false;
        std::vector<RecordedInput> inputs;
        std::vector<std::pair<std::size_t, std::size_t>> buffer_versions;
        std::vector<RecordedInput> last_repeatable;
        std::size_t replay_depth = 0;
        std::size_t group_depth = 0;
        std::size_t group_repeat_count = 1;
        std::vector<RecordedInput> group_inputs;

        bool can_start() const;
        void begin(const EditorState &state);
        void reset();
        bool is_complete(const EditorState &state) const;
        bool changed_buffer(const EditorState &state) const;
        void finalize(EditorState &state);
        void record_group_input(const std::string &token, wint_t key, bool printable);
        void record_command_input(const std::string &token, wint_t key, bool printable);
        void replay_group_inputs(EditorState &state, const std::vector<RecordedInput> &inputs, std::size_t repeat);
        bool repeat_last_command(EditorState &state);
    };

    struct JumpStackState {
        std::vector<JumpLocation> back;
        std::vector<JumpLocation> forward;
    };

    struct CommandHistoryState {
        PromptHistory editor_commands;
        PromptHistory filter_commands;
        PromptHistory sed_commands;
        PromptHistory searches;

        PromptHistory &get_for_kind(CommandPromptKind kind) {
            switch (kind) {
                case CommandPromptKind::EditorCommand: return editor_commands;
                case CommandPromptKind::FilterSelection: return filter_commands;
                case CommandPromptKind::SedSelection: return sed_commands;
            }
            return editor_commands; // Should never reach here
        }

        const PromptHistory &get_for_kind(CommandPromptKind kind) const {
            switch (kind) {
                case CommandPromptKind::EditorCommand: return editor_commands;
                case CommandPromptKind::FilterSelection: return filter_commands;
                case CommandPromptKind::SedSelection: return sed_commands;
            }
            return editor_commands; // Should never reach here
        }
    };

    EditorSession session;
    EditorRuntime runtime;
    EditorConfig config;
    KeyBindings keybindings;
    Theme theme{};
    WindowManager windows;
    std::map<std::size_t, WindowUiState> window_ui_map;
    std::map<std::size_t, BufferUiState> buffer_ui_map;
    std::map<std::size_t, SyntaxUiState> syntax_ui_map;
    bool should_quit = false;
    Mode mode = Mode::Normal;
    CommandPromptKind command_prompt_kind = CommandPromptKind::EditorCommand;
    std::u32string command_buffer;
    std::size_t prompt_cursor = 0;
    CommandHistoryState command_history;
    std::u32string search_buffer;
    std::string status_message = "NORMAL";
    PopupState popup;
    bool diagnostics_visible = true;
    bool insert_session_active = false;
    PendingInputState pending;
    CommandRecordingState recording;
    JumpStackState jump_stack;
    EditorControlServer control_server;

    // Member functions
    EditorWindow &active_window();
    const EditorWindow &active_window() const;
    EditorBuffer &window_buffer(std::size_t window_id);
    const EditorBuffer &window_buffer(std::size_t window_id) const;
    EditorBuffer &active_buffer();
    const EditorBuffer &active_buffer() const;
    EditorCore &active_core();
    const EditorCore &active_core() const;
    EditorCore &window_core(std::size_t window_id);
    const EditorCore &window_core(std::size_t window_id) const;
    WindowUiState &window_ui(std::size_t window_id);
    const WindowUiState &window_ui(std::size_t window_id) const;
    SyntaxUiState &buffer_syntax_ui(std::size_t buffer_id);
    const SyntaxUiState &buffer_syntax_ui(std::size_t buffer_id) const;
    BufferUiState &buffer_ui_state(std::size_t buffer_id);
    const BufferUiState &buffer_ui_state(std::size_t buffer_id) const;
    BufferUiState &active_buffer_cache();
    const BufferUiState &active_buffer_cache() const;
    WindowUiState &active_buffer_ui();
    const WindowUiState &active_buffer_ui() const;
    Position displayed_cursor(std::size_t window_id) const;
    std::optional<Range> displayed_selection_range(std::size_t window_id) const;
    void sync_window_view_from_core(std::size_t window_id);
    void sync_core_view_from_window(std::size_t window_id);
    PromptHistory &active_prompt_history();
    void set_status(const std::string &message);
    void show_popup(std::string title, std::u32string text);
    void dismiss_popup();
    void begin_insert_session();
    void end_insert_session();
    void sync_active_window_buffer();
    void show_buffer_in_active_window(std::size_t buffer_id, bool reset_view = true);
    void focus_window(std::size_t window_id);
    void enter_normal_mode();
    void enter_insert_mode();
    void enter_command_mode();
    void enter_filter_command_mode();
    void enter_sed_command_mode();
    void enter_search_mode();
    void quit_editor();
    void normalize_selected_diagnostic();
    void navigate_diagnostic(bool forward);
    void request_definition();
    void request_hover();
    void request_completion();
    void select_enclosing_ast();
    void select_inner_ast();
    void refresh_search_matches(bool move_to_best_match);
    void navigate_search_match(bool forward);
    // Priority 2 member functions
    void show_diagnostics_summary();
    void show_lsp_status();
    void show_tree_sitter_status();
    void set_search_status(const std::string &suffix = "");
    void show_command_completion();
    void execute_command();
    bool handle_popup_input(const std::string &token);
    void add_prompt_history_entry(const std::u32string &entry);
    void browse_prompt_history(bool previous);
    bool should_render_diagnostics(std::size_t window_id) const;
    void refresh_search_matches_for_window(std::size_t window_id);
    void handle_edit_command(const std::string &argument);
    bool reload_editor_configuration(std::string &error_message);
    void open_startup_file_picker(const std::optional<std::filesystem::path> &root = std::nullopt);
    void open_file_under_cursor();
    bool popup_accepts_input() const;
    bool split_active_window(WindowSplitDirection direction);
    bool close_active_window();
    bool close_other_windows();
    void handle_input();
    void handle_service_events();
    void refresh_syntax_highlights(std::size_t window_id);
    // Phase 1: Command handler member functions
    void handle_quit_command(bool force);
    void handle_write_command(const std::string &argument);
    void handle_write_quit_command(const std::string &argument);
    void handle_buffer_switch_command(const std::string &argument);
    void handle_buffer_delete_command(bool force);
    void handle_goto_line_command(const std::string &argument);
    void handle_pick_theme_command();
    void handle_find_file_command();
    void handle_grep_command(const std::string &argument);
    void execute_filter_command();
    void execute_sed_command();
    void handle_substitute_command(const SubstituteCommand &command);
    std::optional<std::string> run_picker_command(const std::string &pipeline_command, std::string &error_message);
};

struct ClickedBufferPosition {
    std::size_t window_id = 0;
    Position position;
};

// Helper wrappers for backwards compatibility
EditorWindow &active_window(EditorState &state);
const EditorWindow &active_window(const EditorState &state);
EditorBuffer &active_buffer(EditorState &state);
const EditorBuffer &active_buffer(const EditorState &state);
std::string buffer_text_utf8(const EditorBuffer &buffer);
JsonValue json_position(Position position);
JsonValue json_range(const Range &range);
const char *selection_mode_name(SelectionMode mode);
JsonValue json_buffer_summary(const EditorState &state, const EditorBuffer &buffer);
// Helper wrappers - these are now member functions, wrappers kept for backwards compatibility
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
std::string make_status_bar_right_text(const EditorCore &core, Position cursor);
// Popup and UI helper functions - Priority 2 migrations
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
void open_file_under_cursor(EditorState &state);
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