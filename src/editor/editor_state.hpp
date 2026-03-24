#pragma once

import editor_core;
import theme;

#include "config.hpp"
#include "control_server.hpp"
#include "editor_commands.hpp"
#include "editor_session.hpp"
#include "editor_windows.hpp"
#include "json.hpp"
#include "lua_runtime.hpp"
#include "position_utils.hpp"
#include "services.hpp"
#include "syntax.hpp"
#include "text_encoding_utils.hpp"
#include "uri_utils.hpp"

#include <chrono>
#include <curses.h>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

import keybindings;

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
    bool is_lua = false;
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

struct WrapSegmentLayout {
    std::size_t start_column = 0;
    std::size_t start_width = 0;
};

using WrapLineLayout = std::vector<WrapSegmentLayout>;

struct WrapLayoutCache {
    int buffer_cols = -1;
    std::size_t tabstop = 0;
    std::size_t text_revision = std::numeric_limits<std::size_t>::max();
    std::vector<WrapLineLayout> lines;
};

struct VisualRowsCache {
    int buffer_cols = -1;
    std::size_t text_revision = std::numeric_limits<std::size_t>::max();
    std::size_t diagnostics_revision = std::numeric_limits<std::size_t>::max();
    std::size_t annotations_revision = std::numeric_limits<std::size_t>::max();
    bool show_diagnostics = true;
    VisualRows rows;
    std::vector<std::size_t> first_visual_row_by_buffer_row;
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
        std::map<std::pair<int, std::size_t>, WrapLayoutCache> wrap_layout_caches;
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
        std::optional<std::size_t> buffer_id;
        std::size_t document_version = 0;
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
            return editor_commands;
        }

        const PromptHistory &get_for_kind(CommandPromptKind kind) const {
            switch (kind) {
                case CommandPromptKind::EditorCommand: return editor_commands;
                case CommandPromptKind::FilterSelection: return filter_commands;
                case CommandPromptKind::SedSelection: return sed_commands;
            }
            return editor_commands;
        }
    };

    struct CommandSelectionSnapshot {
        std::size_t buffer_id = 0;
        Range range;
    };

    struct PanelState {
        std::optional<std::size_t> window_id;
        std::optional<std::size_t> buffer_id;
        bool visible = false;
        bool follow_output = true;
    };

    EditorSession session;
    EditorRuntime runtime;
    LuaRuntime lua;
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
    bool pending_config_reload = false;
    bool key_inspector_armed = false;
    std::optional<CommandSelectionSnapshot> command_selection_snapshot;
    PendingInputState pending;
    CommandRecordingState recording;
    JumpStackState jump_stack;
    std::map<std::string, std::size_t> named_special_buffers;
    PanelState panel;
    std::optional<std::filesystem::path> input_corpus_path;
    std::unique_ptr<std::ofstream> input_corpus_stream;
    EditorControlServer control_server;

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
    void capture_command_selection_snapshot();
    void clear_command_selection_snapshot();
    std::optional<CommandSelectionSnapshot> selection_snapshot_for_commands() const;
    std::optional<std::u32string> selection_text_for_commands() const;
    bool replace_selection_for_commands(const std::u32string &text);
    bool buffer_is_user_editable(std::size_t buffer_id) const;
    bool active_buffer_is_user_editable() const;
    EditorBuffer &ensure_named_special_buffer(
        const std::string &name,
        EditorBufferKind kind,
        bool editable = false,
        bool activate = false);
    bool panel_has_buffer() const;
    bool panel_is_visible() const;
    bool active_window_is_panel() const;
    void set_panel_follow_output(bool follow);
    void append_to_buffer(std::size_t buffer_id, const std::u32string &text, bool move_cursor_to_end = false);
    void clear_buffer_contents(std::size_t buffer_id);
    void sync_window_view_from_core(std::size_t window_id);
    void sync_core_view_from_window(std::size_t window_id);
    PromptHistory &active_prompt_history();
    void set_status(const std::string &message);
    void show_popup(std::string title, std::u32string text);
    void dismiss_popup();
    void begin_insert_session();
    void end_insert_session();
    void reconcile_closed_buffer(std::size_t closed_buffer_id, std::size_t replacement_buffer_id);
    void sync_active_window_buffer();
    void show_buffer_in_active_window(std::size_t buffer_id, bool reset_view = true);
    void show_buffer_in_panel(std::size_t buffer_id, bool focus_panel = false);
    bool close_panel(bool preserve_buffer = true);
    bool toggle_panel();
    bool focus_panel();
    bool clear_panel();
    bool initialize_input_corpus_recording(const std::filesystem::path &directory, std::string &error_message);
    void record_input_corpus_event(wint_t key, bool is_special);
    void focus_window(std::size_t window_id);
    void enter_normal_mode(bool preserve_status = false);
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
    void show_diagnostics_summary();
    void show_lsp_status();
    void show_tree_sitter_status();
    void request_config_reload();
    bool apply_pending_config_reload();
    void set_search_status(const std::string &suffix = "");
    void show_command_completion();
    void execute_command();
    void execute_command_text(const std::string &command);
    void dispatch_editor_event(const EditorEvent &event);
    void dispatch_editor_events(EditorCore &core);
    bool handle_popup_input(const std::string &token);
    void add_prompt_history_entry(const std::u32string &entry);
    void browse_prompt_history(bool previous);
    bool should_render_diagnostics(std::size_t window_id) const;
    void refresh_search_matches_for_window(std::size_t window_id);
    void handle_edit_command(const std::string &argument);
    void handle_lua_command(const std::string &argument);
    bool reload_editor_configuration(std::string &error_message);
    void open_startup_file_picker(const std::optional<std::filesystem::path> &root = std::nullopt);
    bool popup_accepts_input() const;
    bool split_active_window(WindowSplitDirection direction);
    bool close_active_window();
    bool close_other_windows();
    void handle_input();
    void handle_service_events();
    void refresh_syntax_highlights(std::size_t window_id);
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
    bool run_filter_command(
        const std::string &command,
        const std::u32string &input_text,
        std::u32string &output_text,
        std::string &error_message);
};
