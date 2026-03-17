#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct Position {
    std::size_t row = 0;
    std::size_t column = 0;

    // Comparison operators - lexicographic order (row first, then column)
    auto operator<=>(const Position &) const = default;
};

struct Range {
    Position start;
    Position end;
};

enum class SelectionMode {
    Character,
    Line,
};

struct EditorViewState {
    Position cursor;
    std::optional<Position> selection_anchor;
    SelectionMode selection_mode = SelectionMode::Character;
    std::size_t preferred_column = 0;
};

enum class StyleRole {
    DefaultText,
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

struct TextStyle {
    short foreground = -1;
    short background = -1;
    bool bold = false;
    bool underline = false;
    bool reverse = false;
};

struct HighlightSpan {
    Range range;
    StyleRole role = StyleRole::DefaultText;
    int priority = 0;
};

struct TextEdit {
    Range range;
    std::u32string text;
};

struct Utf16Position {
    std::size_t row = 0;
    std::size_t column = 0;
};

enum class DiagnosticSeverity {
    Error,
    Warning,
};

enum class AnnotationSeverity {
    Info,
    Warning,
    Error,
};

enum class AnnotationKind {
    Note,
    Diagnostic,
};

struct Diagnostic {
    Range range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string source;
    std::u32string message;
};

struct InlineAnnotation {
    Range range;
    AnnotationSeverity severity = AnnotationSeverity::Info;
    AnnotationKind kind = AnnotationKind::Note;
    std::string source;
    std::u32string text;
};

enum class EditorEventType {
    DocumentOpened,
    DocumentChanged,
    DocumentSaved,
    DocumentClosed,
    CursorMoved,
    DiagnosticsChanged,
    AnnotationsChanged,
};

struct EditorEvent {
    EditorEventType type = EditorEventType::DocumentChanged;
    std::string document_uri;
    std::size_t document_version = 0;
    Position cursor;
    std::optional<Range> range;
    std::u32string text;
};

class EditorCore;

struct EditCommand {
    virtual ~EditCommand() = default;
    virtual void apply(EditorCore &core) = 0;
    virtual void undo(EditorCore &core) = 0;
    virtual const char *name() const = 0;
};

struct EditorCommandAccess {
    static void insert_codepoint(EditorCore &core, Position position, char32_t codepoint);
    static void remove_codepoint(EditorCore &core, Position position);
    static void split_line(EditorCore &core, Position position);
    static void join_with_previous(EditorCore &core, std::size_t row);
    static void join_with_next(EditorCore &core, std::size_t row);
    static void insert_line(EditorCore &core, std::size_t row, const std::u32string &line);
    static std::u32string remove_line(EditorCore &core, std::size_t row);
    static const std::u32string &line(const EditorCore &core, std::size_t row);
    static void replace_line(EditorCore &core, std::size_t row, const std::u32string &line);
    static void insert_text(EditorCore &core, Position position, const std::u32string &text);
    static std::u32string delete_range(EditorCore &core, Range range);
    static Position position_after_text(const EditorCore &core, Position position, const std::u32string &text);
};

// Utility functions extracted to separate modules
#include "text_encoding_utils.hpp"
#include "uri_utils.hpp"
#include "position_utils.hpp"

class EditorCore {
  public:
    EditorCore();
    EditorCore(const EditorCore &) = delete;
    EditorCore &operator=(const EditorCore &) = delete;
    EditorCore(EditorCore &&) = default;
    EditorCore &operator=(EditorCore &&) = default;

    const std::vector<std::u32string> &lines() const;
    const std::optional<std::string> &file_path() const;
    std::string display_file_name() const;
    std::string document_uri() const;
    Position cursor() const;
    EditorViewState view_state() const;
    void restore_view_state(const EditorViewState &view_state, bool emit_cursor_event = false);
    Range line_range(std::size_t row) const;
    std::size_t line_count() const;
    std::size_t line_length(std::size_t row) const;
    std::size_t current_revision() const;
    std::size_t saved_revision() const;
    std::size_t document_version() const;
    std::size_t saved_document_version() const;
    std::size_t diagnostics_revision() const;
    std::size_t annotations_revision() const;
    bool is_dirty() const;
    bool has_selection() const;
    std::optional<Position> selection_anchor() const;
    SelectionMode selection_mode() const;
    std::optional<Range> selection_range() const;
    std::u32string yank_buffer() const;
    SelectionMode yank_mode() const;
    void set_yank_buffer(std::u32string text, SelectionMode mode);
    const std::vector<Diagnostic> &diagnostics() const;
    const std::vector<Diagnostic> &document_diagnostics(const std::string &document_uri) const;
    const std::vector<InlineAnnotation> &annotations() const;
    const std::vector<InlineAnnotation> &document_annotations(const std::string &document_uri) const;
    std::vector<InlineAnnotation> projected_annotations() const;
    const std::vector<EditorEvent> &pending_events() const;
    std::vector<EditorEvent> take_events();

    void set_cursor(Position position);
    void move_left();
    void move_right();
    void move_up();
    void move_down();
    void move_by_lines(int delta);
    void move_line_start();
    void move_line_end();
    void move_to_first_line();
    void move_to_last_line();
    bool move_to_character_forward(char32_t target, bool inclusive);
    bool move_to_character_backward(char32_t target, bool inclusive);
    void begin_selection(SelectionMode mode = SelectionMode::Character);
    void clear_selection();
    bool set_selection_range(Range range, SelectionMode mode = SelectionMode::Character);
    bool extend_selection_to_range(Range range);

    bool load_file(const std::string &path);
    void open_empty_file(const std::string &path);
    bool save_current_file();
    bool save_current_file_as(const std::string &path);
    void close_document();
    std::u32string read_text(Range range) const;
    bool delete_range(Range range);
    bool replace_range(Range range, const std::u32string &text);
    bool insert_text(Position position, const std::u32string &text);
    bool apply_text_edits(const std::vector<TextEdit> &edits);
    std::size_t utf8_offset_for_position(Position position) const;
    Position position_for_utf8_offset(std::size_t offset) const;
    Utf16Position utf16_position_for_position(Position position) const;
    Position position_for_utf16(Utf16Position position) const;
    std::optional<Range> inner_word_range() const;
    std::optional<Range> a_word_range() const;

    void insert_codepoint(char32_t codepoint);
    bool insert_soft_tab(std::size_t tabstop, std::size_t softtabstop, bool expandtab);
    void insert_newline();
    void insert_newline_with_autoindent();
    void backspace_character();
    bool outdent_before_cursor(std::size_t tabstop, std::size_t softtabstop);
    void delete_character_under_cursor();
    void delete_current_line();
    void open_line_below();
    void open_line_below_with_autoindent();
    void open_line_above();
    void open_line_above_with_autoindent();
    bool indent_lines(std::size_t start_row, std::size_t end_row, std::size_t shiftwidth, bool expandtab, std::size_t tabstop);
    bool outdent_lines(std::size_t start_row, std::size_t end_row, std::size_t shiftwidth, std::size_t tabstop);
    std::size_t substitute_regex(
        std::size_t start_row,
        std::size_t end_row,
        const std::string &pattern,
        const std::string &replacement,
        bool global,
        std::string &error_message);
    bool yank_selection();
    bool delete_selection();
    bool paste_before_cursor();
    bool paste_after_cursor();
    bool replace_selection_with_yank();
    bool undo();
    bool redo();
    void begin_compound_edit();
    void end_compound_edit();
    void set_diagnostics(std::vector<Diagnostic> diagnostics);
    void clear_diagnostics();
    void set_document_diagnostics(const std::string &document_uri, std::vector<Diagnostic> diagnostics);
    void clear_document_diagnostics(const std::string &document_uri);
    void set_annotations(std::vector<InlineAnnotation> annotations);
    void clear_annotations();
    void set_document_annotations(const std::string &document_uri, std::vector<InlineAnnotation> annotations);
    void clear_document_annotations(const std::string &document_uri);

  private:
    friend struct EditorCommandAccess;

    std::vector<std::u32string> lines_;
    std::optional<std::string> file_path_;
    Position cursor_;
    std::optional<Position> selection_anchor_;
    SelectionMode selection_mode_ = SelectionMode::Character;
    std::size_t preferred_column_ = 0;
    std::vector<std::unique_ptr<EditCommand>> undo_stack_;
    std::vector<std::unique_ptr<EditCommand>> redo_stack_;
    std::vector<std::unique_ptr<EditCommand>> compound_commands_;
    std::vector<std::u32string> compound_before_lines_;
    Position compound_before_cursor_;
    std::size_t compound_depth_ = 0;
    std::size_t current_revision_ = 0;
    std::size_t saved_revision_ = 0;
    std::size_t document_version_ = 0;
    std::size_t saved_document_version_ = 0;
    std::size_t diagnostics_revision_ = 0;
    std::size_t annotations_revision_ = 0;
    std::u32string yank_buffer_;
    SelectionMode yank_mode_ = SelectionMode::Character;
    std::string document_uri_;
    std::uint64_t untitled_id_ = 0;
    std::map<std::string, std::vector<Diagnostic>> diagnostics_by_uri_;
    std::map<std::string, std::vector<InlineAnnotation>> annotations_by_uri_;
    std::vector<EditorEvent> pending_events_;
    bool suppress_cursor_events_ = false;

    void ensure_buffer_not_empty();
    void clamp_cursor();
    Position clamped_position(Position position) const;
    void emit_event(EditorEvent event);
    void emit_document_closed(const std::string &document_uri, std::size_t document_version);
    void emit_document_opened();
    void emit_document_saved();
    void emit_document_changed(const std::vector<std::u32string> &before_lines);
    void emit_document_changed(Range range, const std::u32string &text);
    void emit_cursor_moved(Position previous_cursor);
    void emit_diagnostics_changed(const std::string &document_uri);
    void emit_annotations_changed(const std::string &document_uri);
    void set_cursor_internal(Position position, bool emit_event);
    void restore_view_state_internal(const EditorViewState &view_state, bool emit_cursor_event);
    void update_preferred_column();
    Position position_after_character(Position position) const;
    Position position_before(Position position) const;
    std::u32string read_range(Range range) const;
    Position position_after_text(Position position, const std::u32string &text) const;
    bool save_file(const std::string &path) const;
    void reset_history();
    void apply_command(std::unique_ptr<EditCommand> command);

    void raw_insert_codepoint(Position position, char32_t codepoint);
    void raw_remove_codepoint(Position position);
    void raw_split_line(Position position);
    void raw_join_with_previous(std::size_t row);
    void raw_join_with_next(std::size_t row);
    void raw_insert_line(std::size_t row, const std::u32string &line);
    std::u32string raw_remove_line(std::size_t row);
    void raw_insert_text(Position position, const std::u32string &text);
    std::u32string raw_delete_range(Range range);
    bool paste_linewise(bool before_cursor);
};
