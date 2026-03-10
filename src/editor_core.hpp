#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct Position {
    std::size_t row = 0;
    std::size_t column = 0;
};

struct Range {
    Position start;
    Position end;
};

enum class SelectionMode {
    Character,
    Line,
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
    SyntaxString,
    SyntaxComment,
    DiagnosticError,
    DiagnosticWarning,
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

std::u32string utf8_to_u32(const std::string &text);
std::string u32_to_utf8(const std::u32string &text);
bool position_less_than(Position left, Position right);
bool positions_equal(Position left, Position right);
Range normalized_range(Range range);
bool range_contains(const Range &range, Position position);

class EditorCore {
  public:
    EditorCore();

    const std::vector<std::u32string> &lines() const;
    const std::optional<std::string> &file_path() const;
    std::string display_file_name() const;
    Position cursor() const;
    Range line_range(std::size_t row) const;
    std::size_t line_count() const;
    std::size_t line_length(std::size_t row) const;
    std::size_t current_revision() const;
    std::size_t saved_revision() const;
    std::size_t document_version() const;
    std::size_t saved_document_version() const;
    bool is_dirty() const;
    bool has_selection() const;
    std::optional<Position> selection_anchor() const;
    SelectionMode selection_mode() const;
    std::optional<Range> selection_range() const;
    std::u32string yank_buffer() const;
    SelectionMode yank_mode() const;

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
    void begin_selection(SelectionMode mode = SelectionMode::Character);
    void clear_selection();

    bool load_file(const std::string &path);
    bool save_current_file();
    bool save_current_file_as(const std::string &path);
    std::u32string read_text(Range range) const;
    bool delete_range(Range range);
    bool replace_range(Range range, const std::u32string &text);
    bool insert_text(Position position, const std::u32string &text);
    bool apply_text_edits(const std::vector<TextEdit> &edits);
    std::size_t utf8_offset_for_position(Position position) const;
    Position position_for_utf8_offset(std::size_t offset) const;
    Utf16Position utf16_position_for_position(Position position) const;
    Position position_for_utf16(Utf16Position position) const;

    void insert_codepoint(char32_t codepoint);
    void insert_newline();
    void backspace_character();
    void delete_character_under_cursor();
    void delete_current_line();
    void open_line_below();
    void open_line_above();
    bool yank_selection();
    bool delete_selection();
    bool paste_before_cursor();
    bool paste_after_cursor();
    bool replace_selection_with_yank();
    bool undo();
    bool redo();

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
    std::size_t current_revision_ = 0;
    std::size_t saved_revision_ = 0;
    std::size_t document_version_ = 0;
    std::size_t saved_document_version_ = 0;
    std::u32string yank_buffer_;
    SelectionMode yank_mode_ = SelectionMode::Character;

    void ensure_buffer_not_empty();
    void clamp_cursor();
    Position clamped_position(Position position) const;
    void update_preferred_column();
    Position position_after_character(Position position) const;
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
