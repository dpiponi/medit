#pragma once

#include "config.hpp"
#include "control_server.hpp"
#include "editor_commands.hpp"
#include "editor_core.hpp"
#include "editor_session.hpp"
#include "editor_windows.hpp"
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

struct VisualRowsCache {
    int buffer_cols = -1;
    std::size_t text_revision = std::numeric_limits<std::size_t>::max();
    std::size_t diagnostics_revision = std::numeric_limits<std::size_t>::max();
    std::size_t annotations_revision = std::numeric_limits<std::size_t>::max();
    bool show_diagnostics = true;
    std::vector<VisualRow> rows;
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
        std::vector<std::vector<HighlightSpan>> syntax_highlights;
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
        std::vector<PopupMenuItem> items;
        std::u32string filter;
        std::vector<std::size_t> filtered_indices;
        std::size_t selected_index = 0;
        std::size_t scroll_offset = 0;
        Mode originating_mode = Mode::Normal;
        PopupApplyTarget apply_target = PopupApplyTarget::BufferText;
        PopupFilterMode filter_mode = PopupFilterMode::ContainsLabelOrDetail;
    };

    EditorSession session;
    EditorRuntime runtime;
    EditorConfig config;
    KeyBindings keybindings;
    Theme theme = load_embedded_theme();
    WindowManager windows;
    std::map<std::size_t, WindowUiState> window_ui;
    std::map<std::size_t, BufferUiState> buffer_ui;
    std::map<std::size_t, SyntaxUiState> syntax_ui;
    bool should_quit = false;
    Mode mode = Mode::Normal;
    CommandPromptKind command_prompt_kind = CommandPromptKind::EditorCommand;
    std::u32string command_buffer;
    PromptHistory editor_command_history;
    PromptHistory filter_command_history;
    PromptHistory sed_command_history;
    PromptHistory search_history;
    std::u32string search_buffer;
    std::string status_message = "NORMAL";
    PopupState popup;
    bool diagnostics_visible = true;
    bool insert_session_active = false;
    std::vector<std::string> pending_tokens;
    PendingMotion pending_motion = PendingMotion::None;
    std::string repeat_digits;
    std::size_t pending_motion_repeat_count = 1;
    std::size_t pending_replace_count = 0;
    std::size_t replay_depth = 0;
    std::size_t group_depth = 0;
    std::size_t group_repeat_count = 1;
    std::vector<RecordedInput> group_inputs;
    bool command_recording = false;
    bool command_recording_nonrepeatable = false;
    std::vector<RecordedInput> command_inputs;
    std::vector<std::pair<std::size_t, std::size_t>> command_buffer_versions;
    std::vector<RecordedInput> last_repeatable_command;
    std::vector<JumpLocation> jump_back_stack;
    std::vector<JumpLocation> jump_forward_stack;
    EditorControlServer control_server;
};

struct ClickedBufferPosition {
    std::size_t window_id = 0;
    Position position;
};

Position displayed_cursor(const EditorState &state, std::size_t window_id);
std::optional<Range> displayed_selection_range(const EditorState &state, std::size_t window_id);

EditorWindow &active_window(EditorState &state);
const EditorWindow &active_window(const EditorState &state);
EditorBuffer &window_buffer(EditorState &state, std::size_t window_id);
const EditorBuffer &window_buffer(const EditorState &state, std::size_t window_id);
EditorBuffer &active_buffer(EditorState &state);
const EditorBuffer &active_buffer(const EditorState &state);
EditorCore &active_core(EditorState &state);
const EditorCore &active_core(const EditorState &state);
EditorCore &window_core(EditorState &state, std::size_t window_id);
const EditorCore &window_core(const EditorState &state, std::size_t window_id);
EditorState::WindowUiState &window_ui(EditorState &state, std::size_t window_id);
const EditorState::WindowUiState &window_ui(const EditorState &state, std::size_t window_id);
EditorState::SyntaxUiState &buffer_syntax_ui(EditorState &state, std::size_t buffer_id);
const EditorState::SyntaxUiState &buffer_syntax_ui(const EditorState &state, std::size_t buffer_id);
EditorState::BufferUiState &buffer_ui_state(EditorState &state, std::size_t buffer_id);
const EditorState::BufferUiState &buffer_ui_state(const EditorState &state, std::size_t buffer_id);
EditorState::BufferUiState &active_buffer_cache(EditorState &state);
const EditorState::BufferUiState &active_buffer_cache(const EditorState &state);
EditorState::WindowUiState &active_buffer_ui(EditorState &state);
const EditorState::WindowUiState &active_buffer_ui(const EditorState &state);
void sync_window_view_from_core(EditorState &state, std::size_t window_id);
std::wstring u32_to_wstring(const std::u32string &text);
int codepoint_width(char32_t codepoint);
std::size_t codepoint_display_width(char32_t codepoint, std::size_t visual_column, std::size_t tabstop);
std::string mode_name(Mode mode);
std::string make_status_bar_left_text(
    const EditorState &state,
    const EditorCore &core,
    const std::string &language,
    const std::string &workspace);
std::string make_status_bar_right_text(const EditorCore &core, Position cursor);
void set_status(EditorState &state, const std::string &message);
std::size_t popup_menu_visible_rows_for_screen(int screen_rows);
bool should_render_diagnostics(const EditorState &state, std::size_t window_id);
void normalize_selected_diagnostic(EditorState &state);
std::vector<std::u32string> wrap_annotation_text(const std::u32string &text, int max_cols);
const std::vector<VisualRow> &visual_rows_for_window(const EditorState &state, std::size_t window_id, int buffer_cols);
std::size_t visual_row_for_buffer_row(const std::vector<VisualRow> &rows, std::size_t buffer_row);
void refresh_search_matches_for_window(EditorState &state, std::size_t window_id);
void restore_shell_terminal_state();
void apply_theme_to_terminal(const Theme &theme);
void setup_terminal(const Theme &theme);
void teardown_terminal();
void suspend_editor(EditorState &state);
int line_number_width(const EditorCore &core);
std::string build_status_text(const EditorState &state);
std::optional<ClickedBufferPosition> buffer_position_from_screen_point(const EditorState &state, int screen_row, int screen_col);
void draw_editor(const EditorState &state);
void refresh_syntax_highlights(EditorState &state, std::size_t window_id);
void render_frame(EditorState &state);
