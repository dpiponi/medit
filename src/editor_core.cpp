#include "editor_core.hpp"

#include <codecvt>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <locale>
#include <sstream>
#include <utility>

namespace {

std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> &utf8_converter() {
    static std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> converter;
    return converter;
}

std::uint64_t next_untitled_id() {
    static std::uint64_t next_id = 1;
    return next_id++;
}

bool is_unreserved_uri_byte(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
        ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

std::string percent_encode_path(const std::string &path) {
    static const char *kHex = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char ch : path) {
        if (is_unreserved_uri_byte(ch)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(ch >> 4) & 0x0F]);
        encoded.push_back(kHex[ch & 0x0F]);
    }
    return encoded;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

std::string percent_decode_path(const std::string &path) {
    std::string decoded;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            int high = hex_value(path[i + 1]);
            int low = hex_value(path[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(path[i]);
    }
    return decoded;
}

std::u32string buffer_text(const std::vector<std::u32string> &lines) {
    std::u32string text;
    for (std::size_t row = 0; row < lines.size(); ++row) {
        text += lines[row];
        if (row + 1 < lines.size()) {
            text.push_back(U'\n');
        }
    }
    return text;
}

Range full_document_range(const std::vector<std::u32string> &lines) {
    if (lines.empty()) {
        return {{0, 0}, {0, 0}};
    }
    std::size_t last_row = lines.size() - 1;
    return {{0, 0}, {last_row, lines[last_row].size()}};
}

struct InsertCodepointCommand : EditCommand {
    Position position;
    char32_t codepoint;

    InsertCodepointCommand(Position position_in, char32_t codepoint_in)
        : position(position_in), codepoint(codepoint_in) {}

    void apply(EditorCore &core) override {
        EditorCommandAccess::insert_codepoint(core, position, codepoint);
    }

    void undo(EditorCore &core) override {
        EditorCommandAccess::remove_codepoint(core, position);
    }

    const char *name() const override {
        return "insert";
    }
};

struct SplitLineCommand : EditCommand {
    Position position;

    explicit SplitLineCommand(Position position_in) : position(position_in) {}

    void apply(EditorCore &core) override {
        EditorCommandAccess::split_line(core, position);
    }

    void undo(EditorCore &core) override {
        EditorCommandAccess::join_with_previous(core, position.row + 1);
    }

    const char *name() const override {
        return "split line";
    }
};

struct BackspaceCommand : EditCommand {
    bool joins_lines = false;
    Position position;
    char32_t deleted_codepoint = U'\0';
    std::u32string removed_line_tail;

    BackspaceCommand(Position position_in, char32_t codepoint_in)
        : joins_lines(false), position(position_in), deleted_codepoint(codepoint_in) {}

    BackspaceCommand(Position position_in, std::u32string removed_line_tail_in)
        : joins_lines(true), position(position_in), removed_line_tail(std::move(removed_line_tail_in)) {}

    void apply(EditorCore &core) override {
        if (joins_lines) {
            EditorCommandAccess::join_with_previous(core, position.row);
            return;
        }
        EditorCommandAccess::remove_codepoint(core, position);
    }

    void undo(EditorCore &core) override {
        if (joins_lines) {
            std::size_t split_column =
                EditorCommandAccess::line(core, position.row - 1).size() - removed_line_tail.size();
            EditorCommandAccess::split_line(core, {position.row - 1, split_column});
            return;
        }
        EditorCommandAccess::insert_codepoint(core, position, deleted_codepoint);
    }

    const char *name() const override {
        return "backspace";
    }
};

struct DeleteCharCommand : EditCommand {
    bool joins_lines = false;
    Position position;
    char32_t deleted_codepoint = U'\0';

    DeleteCharCommand(Position position_in, char32_t codepoint_in)
        : joins_lines(false), position(position_in), deleted_codepoint(codepoint_in) {}

    explicit DeleteCharCommand(Position position_in)
        : joins_lines(true), position(position_in) {}

    void apply(EditorCore &core) override {
        if (joins_lines) {
            EditorCommandAccess::join_with_next(core, position.row);
            core.set_cursor(position);
            return;
        }
        EditorCommandAccess::remove_codepoint(core, position);
    }

    void undo(EditorCore &core) override {
        if (joins_lines) {
            EditorCommandAccess::split_line(core, position);
            core.set_cursor(position);
            return;
        }
        EditorCommandAccess::insert_codepoint(core, position, deleted_codepoint);
        core.set_cursor(position);
    }

    const char *name() const override {
        return "delete char";
    }
};

struct DeleteLineCommand : EditCommand {
    std::size_t row;
    std::size_t cursor_column_before;
    std::u32string removed_line;
    bool was_only_line;

    DeleteLineCommand(std::size_t row_in, std::size_t cursor_column_before_in, std::u32string removed_line_in, bool was_only_line_in)
        : row(row_in),
          cursor_column_before(cursor_column_before_in),
          removed_line(std::move(removed_line_in)),
          was_only_line(was_only_line_in) {}

    void apply(EditorCore &core) override {
        EditorCommandAccess::remove_line(core, row);
    }

    void undo(EditorCore &core) override {
        if (was_only_line) {
            EditorCommandAccess::replace_line(core, 0, removed_line);
            core.set_cursor({0, cursor_column_before});
            return;
        }
        EditorCommandAccess::insert_line(core, row, removed_line);
        core.set_cursor({row, cursor_column_before});
    }

    const char *name() const override {
        return "delete line";
    }
};

struct InsertLineCommand : EditCommand {
    std::size_t row;

    explicit InsertLineCommand(std::size_t row_in) : row(row_in) {}

    void apply(EditorCore &core) override {
        EditorCommandAccess::insert_line(core, row, U"");
    }

    void undo(EditorCore &core) override {
        EditorCommandAccess::remove_line(core, row);
        std::size_t target_row = row > 0 ? row - 1 : 0;
        core.set_cursor({target_row, 0});
    }

    const char *name() const override {
        return "insert line";
    }
};

struct ReplaceRangeCommand : EditCommand {
    Range range;
    std::u32string replaced_text;
    std::u32string inserted_text;

    ReplaceRangeCommand(Range range_in, std::u32string replaced_text_in, std::u32string inserted_text_in)
        : range(normalized_range(range_in)),
          replaced_text(std::move(replaced_text_in)),
          inserted_text(std::move(inserted_text_in)) {}

    void apply(EditorCore &core) override {
        EditorCommandAccess::delete_range(core, range);
        EditorCommandAccess::insert_text(core, range.start, inserted_text);
    }

    void undo(EditorCore &core) override {
        Range inserted_range{range.start, range.start};
        inserted_range.end = EditorCommandAccess::position_after_text(core, range.start, inserted_text);
        EditorCommandAccess::delete_range(core, inserted_range);
        EditorCommandAccess::insert_text(core, range.start, replaced_text);
    }

    const char *name() const override {
        return "replace range";
    }
};

struct TransactionCommand : EditCommand {
    std::vector<ReplaceRangeCommand> commands;

    explicit TransactionCommand(std::vector<ReplaceRangeCommand> commands_in) : commands(std::move(commands_in)) {}

    void apply(EditorCore &core) override {
        for (auto &command : commands) {
            command.apply(core);
        }
    }

    void undo(EditorCore &core) override {
        for (auto it = commands.rbegin(); it != commands.rend(); ++it) {
            it->undo(core);
        }
    }

    const char *name() const override {
        return "transaction";
    }
};

struct CompoundCommand : EditCommand {
    std::vector<std::unique_ptr<EditCommand>> commands;

    explicit CompoundCommand(std::vector<std::unique_ptr<EditCommand>> commands_in) : commands(std::move(commands_in)) {}

    void apply(EditorCore &core) override {
        for (auto &command : commands) {
            command->apply(core);
        }
    }

    void undo(EditorCore &core) override {
        for (auto it = commands.rbegin(); it != commands.rend(); ++it) {
            (*it)->undo(core);
        }
    }

    const char *name() const override {
        return "compound";
    }
};

}  // namespace

std::string file_uri_for_path(const std::string &path) {
    std::filesystem::path absolute = std::filesystem::absolute(path);
    return "file://" + percent_encode_path(absolute.string());
}

std::string file_path_from_uri(const std::string &uri) {
    if (uri.rfind("file://", 0) != 0) {
        return "";
    }
    return percent_decode_path(uri.substr(7));
}

std::string normalize_document_uri(const std::string &uri) {
    if (uri.rfind("file://", 0) != 0) {
        return uri;
    }
    std::string path = file_path_from_uri(uri);
    if (path.empty()) {
        return uri;
    }
    return file_uri_for_path(path);
}

std::u32string utf8_to_u32(const std::string &text) {
    return utf8_converter().from_bytes(text);
}

std::string u32_to_utf8(const std::u32string &text) {
    return utf8_converter().to_bytes(text);
}

bool position_less_than(Position left, Position right) {
    if (left.row != right.row) {
        return left.row < right.row;
    }
    return left.column < right.column;
}

bool positions_equal(Position left, Position right) {
    return left.row == right.row && left.column == right.column;
}

Range normalized_range(Range range) {
    if (position_less_than(range.end, range.start)) {
        std::swap(range.start, range.end);
    }
    return range;
}

bool range_contains(const Range &range, Position position) {
    Range normalized = normalized_range(range);
    return !position_less_than(position, normalized.start) && position_less_than(position, normalized.end);
}

bool ranges_overlap(const Range &left, const Range &right) {
    Range normalized_left = normalized_range(left);
    Range normalized_right = normalized_range(right);
    return position_less_than(normalized_left.start, normalized_right.end) &&
           position_less_than(normalized_right.start, normalized_left.end);
}

bool ends_with_newline(const std::u32string &text) {
    return !text.empty() && text.back() == U'\n';
}

std::size_t utf16_units_for_codepoint(char32_t codepoint) {
    return codepoint > 0xFFFF ? 2 : 1;
}

bool is_space_codepoint(char32_t codepoint) {
    wchar_t wide = static_cast<wchar_t>(codepoint);
    return std::iswspace(wide) != 0;
}

bool is_word_codepoint(char32_t codepoint) {
    wchar_t wide = static_cast<wchar_t>(codepoint);
    return codepoint == U'_' || std::iswalnum(wide) != 0;
}

void EditorCommandAccess::insert_codepoint(EditorCore &core, Position position, char32_t codepoint) {
    core.raw_insert_codepoint(position, codepoint);
}

void EditorCommandAccess::remove_codepoint(EditorCore &core, Position position) {
    core.raw_remove_codepoint(position);
}

void EditorCommandAccess::split_line(EditorCore &core, Position position) {
    core.raw_split_line(position);
}

void EditorCommandAccess::join_with_previous(EditorCore &core, std::size_t row) {
    core.raw_join_with_previous(row);
}

void EditorCommandAccess::join_with_next(EditorCore &core, std::size_t row) {
    core.raw_join_with_next(row);
}

void EditorCommandAccess::insert_line(EditorCore &core, std::size_t row, const std::u32string &line) {
    core.raw_insert_line(row, line);
}

std::u32string EditorCommandAccess::remove_line(EditorCore &core, std::size_t row) {
    return core.raw_remove_line(row);
}

const std::u32string &EditorCommandAccess::line(const EditorCore &core, std::size_t row) {
    return core.lines_[row];
}

void EditorCommandAccess::replace_line(EditorCore &core, std::size_t row, const std::u32string &line) {
    core.lines_[row] = line;
}

void EditorCommandAccess::insert_text(EditorCore &core, Position position, const std::u32string &text) {
    core.raw_insert_text(position, text);
}

std::u32string EditorCommandAccess::delete_range(EditorCore &core, Range range) {
    return core.raw_delete_range(range);
}

Position EditorCommandAccess::position_after_text(
    const EditorCore &core,
    Position position,
    const std::u32string &text) {
    return core.position_after_text(position, text);
}

EditorCore::EditorCore() {
    lines_.push_back(U"");
    untitled_id_ = next_untitled_id();
    document_uri_ = "untitled://medit/" + std::to_string(untitled_id_);
}

const std::vector<std::u32string> &EditorCore::lines() const {
    return lines_;
}

const std::optional<std::string> &EditorCore::file_path() const {
    return file_path_;
}

std::string EditorCore::display_file_name() const {
    return file_path_ ? *file_path_ : "[No Name]";
}

std::string EditorCore::document_uri() const {
    return document_uri_;
}

Position EditorCore::cursor() const {
    return cursor_;
}

const std::vector<Diagnostic> &EditorCore::diagnostics() const {
    return document_diagnostics(document_uri_);
}

const std::vector<Diagnostic> &EditorCore::document_diagnostics(const std::string &document_uri) const {
    static const std::vector<Diagnostic> kEmptyDiagnostics;
    auto found = diagnostics_by_uri_.find(document_uri);
    if (found == diagnostics_by_uri_.end()) {
        return kEmptyDiagnostics;
    }
    return found->second;
}

const std::vector<InlineAnnotation> &EditorCore::annotations() const {
    return document_annotations(document_uri_);
}

const std::vector<InlineAnnotation> &EditorCore::document_annotations(const std::string &document_uri) const {
    static const std::vector<InlineAnnotation> kEmptyAnnotations;
    auto found = annotations_by_uri_.find(document_uri);
    if (found == annotations_by_uri_.end()) {
        return kEmptyAnnotations;
    }
    return found->second;
}

std::vector<InlineAnnotation> EditorCore::projected_annotations() const {
    std::vector<InlineAnnotation> projected = annotations();
    for (const Diagnostic &diagnostic : diagnostics()) {
        AnnotationSeverity severity =
            diagnostic.severity == DiagnosticSeverity::Error ? AnnotationSeverity::Error : AnnotationSeverity::Warning;
        projected.push_back({diagnostic.range, severity, AnnotationKind::Diagnostic, diagnostic.source, diagnostic.message});
    }
    return projected;
}

bool EditorCore::has_selection() const {
    return selection_anchor_.has_value();
}

std::optional<Position> EditorCore::selection_anchor() const {
    return selection_anchor_;
}

SelectionMode EditorCore::selection_mode() const {
    return selection_mode_;
}

std::optional<Range> EditorCore::selection_range() const {
    if (!selection_anchor_) {
        return std::nullopt;
    }
    Position anchor = *selection_anchor_;
    if (selection_mode_ == SelectionMode::Line) {
        std::size_t start_row = anchor.row < cursor_.row ? anchor.row : cursor_.row;
        std::size_t end_row = anchor.row > cursor_.row ? anchor.row : cursor_.row;
        Position start{start_row, 0};
        Position end;
        if (end_row + 1 < lines_.size()) {
            end = {end_row + 1, 0};
        } else {
            end = {end_row, line_length(end_row)};
        }
        return Range{start, end};
    }
    Position cursor_extent = position_after_character(cursor_);
    Position anchor_extent = position_after_character(anchor);
    if (position_less_than(cursor_, anchor)) {
        return Range{cursor_, anchor_extent};
    }
    return Range{anchor, cursor_extent};
}

std::u32string EditorCore::yank_buffer() const {
    return yank_buffer_;
}

SelectionMode EditorCore::yank_mode() const {
    return yank_mode_;
}

const std::vector<EditorEvent> &EditorCore::pending_events() const {
    return pending_events_;
}

std::vector<EditorEvent> EditorCore::take_events() {
    std::vector<EditorEvent> events = std::move(pending_events_);
    pending_events_.clear();
    return events;
}

Range EditorCore::line_range(std::size_t row) const {
    return {{row, 0}, {row, line_length(row)}};
}

std::size_t EditorCore::line_count() const {
    return lines_.size();
}

std::size_t EditorCore::line_length(std::size_t row) const {
    return lines_.at(row).size();
}

std::size_t EditorCore::current_revision() const {
    return current_revision_;
}

std::size_t EditorCore::saved_revision() const {
    return saved_revision_;
}

std::size_t EditorCore::document_version() const {
    return document_version_;
}

std::size_t EditorCore::saved_document_version() const {
    return saved_document_version_;
}

bool EditorCore::is_dirty() const {
    return current_revision_ != saved_revision_;
}

void EditorCore::set_cursor(Position position) {
    set_cursor_internal(position, true);
}

Position EditorCore::position_after_character(Position position) const {
    std::size_t line_len = line_length(position.row);
    if (position.column < line_len) {
        return {position.row, position.column + 1};
    }
    return position;
}

Position EditorCore::position_before(Position position) const {
    position = clamped_position(position);
    if (position.column > 0) {
        return {position.row, position.column - 1};
    }
    if (position.row > 0) {
        std::size_t previous_row = position.row - 1;
        return {previous_row, line_length(previous_row)};
    }
    return {0, 0};
}

void EditorCore::ensure_buffer_not_empty() {
    if (lines_.empty()) {
        lines_.push_back(U"");
    }
}

void EditorCore::clamp_cursor() {
    ensure_buffer_not_empty();
    if (cursor_.row >= lines_.size()) {
        cursor_.row = lines_.size() - 1;
    }
    std::size_t max_column = line_length(cursor_.row);
    if (cursor_.column > max_column) {
        cursor_.column = max_column;
    }
}

Position EditorCore::clamped_position(Position position) const {
    if (lines_.empty()) {
        return {0, 0};
    }
    if (position.row >= lines_.size()) {
        position.row = lines_.size() - 1;
    }
    std::size_t max_column = line_length(position.row);
    if (position.column > max_column) {
        position.column = max_column;
    }
    return position;
}

void EditorCore::emit_event(EditorEvent event) {
    pending_events_.push_back(std::move(event));
}

void EditorCore::emit_document_closed(const std::string &document_uri, std::size_t document_version) {
    emit_event({EditorEventType::DocumentClosed, document_uri, document_version, cursor_, std::nullopt, U""});
}

void EditorCore::emit_document_opened() {
    emit_event({
        EditorEventType::DocumentOpened,
        document_uri_,
        document_version_,
        cursor_,
        full_document_range(lines_),
        buffer_text(lines_),
    });
}

void EditorCore::emit_document_saved() {
    emit_event({EditorEventType::DocumentSaved, document_uri_, document_version_, cursor_, std::nullopt, U""});
}

void EditorCore::emit_document_changed(const std::vector<std::u32string> &before_lines) {
    emit_event({
        EditorEventType::DocumentChanged,
        document_uri_,
        document_version_,
        cursor_,
        full_document_range(before_lines),
        buffer_text(lines_),
    });
}

void EditorCore::emit_cursor_moved(Position previous_cursor) {
    if (suppress_cursor_events_ || positions_equal(previous_cursor, cursor_)) {
        return;
    }
    emit_event({EditorEventType::CursorMoved, document_uri_, document_version_, cursor_, std::nullopt, U""});
}

void EditorCore::emit_diagnostics_changed(const std::string &document_uri) {
    emit_event({EditorEventType::DiagnosticsChanged, document_uri, document_version_, cursor_, std::nullopt, U""});
}

void EditorCore::emit_annotations_changed(const std::string &document_uri) {
    emit_event({EditorEventType::AnnotationsChanged, document_uri, document_version_, cursor_, std::nullopt, U""});
}

void EditorCore::set_cursor_internal(Position position, bool emit_cursor_event) {
    Position previous_cursor = cursor_;
    cursor_ = position;
    clamp_cursor();
    update_preferred_column();
    if (emit_cursor_event) {
        emit_cursor_moved(previous_cursor);
    }
}

void EditorCore::update_preferred_column() {
    preferred_column_ = cursor_.column;
}

void EditorCore::move_left() {
    Position previous_cursor = cursor_;
    if (cursor_.column > 0) {
        --cursor_.column;
    } else if (cursor_.row > 0) {
        --cursor_.row;
        cursor_.column = line_length(cursor_.row);
    }
    update_preferred_column();
    emit_cursor_moved(previous_cursor);
}

void EditorCore::move_right() {
    Position previous_cursor = cursor_;
    std::size_t max_column = line_length(cursor_.row);
    if (cursor_.column < max_column) {
        ++cursor_.column;
    } else if (cursor_.row + 1 < lines_.size()) {
        ++cursor_.row;
        cursor_.column = 0;
    }
    update_preferred_column();
    emit_cursor_moved(previous_cursor);
}

void EditorCore::move_up() {
    Position previous_cursor = cursor_;
    if (cursor_.row > 0) {
        --cursor_.row;
        clamp_cursor();
        if (cursor_.column > preferred_column_) {
            cursor_.column = preferred_column_;
        }
    }
    emit_cursor_moved(previous_cursor);
}

void EditorCore::move_down() {
    Position previous_cursor = cursor_;
    if (cursor_.row + 1 < lines_.size()) {
        ++cursor_.row;
        clamp_cursor();
        if (cursor_.column > preferred_column_) {
            cursor_.column = preferred_column_;
        }
    }
    emit_cursor_moved(previous_cursor);
}

void EditorCore::move_by_lines(int delta) {
    Position previous_cursor = cursor_;
    if (delta == 0) {
        return;
    }

    if (delta < 0) {
        std::size_t amount = static_cast<std::size_t>(-delta);
        cursor_.row = amount > cursor_.row ? 0 : cursor_.row - amount;
    } else {
        std::size_t amount = static_cast<std::size_t>(delta);
        std::size_t last_row = lines_.empty() ? 0 : lines_.size() - 1;
        cursor_.row = cursor_.row + amount > last_row ? last_row : cursor_.row + amount;
    }

    clamp_cursor();
    if (cursor_.column > preferred_column_) {
        cursor_.column = preferred_column_;
    }
    emit_cursor_moved(previous_cursor);
}

void EditorCore::move_line_start() {
    Position previous_cursor = cursor_;
    cursor_.column = 0;
    update_preferred_column();
    emit_cursor_moved(previous_cursor);
}

void EditorCore::move_line_end() {
    Position previous_cursor = cursor_;
    cursor_.column = line_length(cursor_.row);
    update_preferred_column();
    emit_cursor_moved(previous_cursor);
}

void EditorCore::move_to_first_line() {
    Position previous_cursor = cursor_;
    cursor_.row = 0;
    clamp_cursor();
    if (cursor_.column > preferred_column_) {
        cursor_.column = preferred_column_;
    }
    emit_cursor_moved(previous_cursor);
}

void EditorCore::move_to_last_line() {
    Position previous_cursor = cursor_;
    cursor_.row = lines_.empty() ? 0 : lines_.size() - 1;
    clamp_cursor();
    if (cursor_.column > preferred_column_) {
        cursor_.column = preferred_column_;
    }
    emit_cursor_moved(previous_cursor);
}

bool EditorCore::move_to_character_forward(char32_t target, bool inclusive) {
    const std::u32string &line = lines_[cursor_.row];
    for (std::size_t column = cursor_.column + 1; column < line.size(); ++column) {
        if (line[column] != target) {
            continue;
        }
        if (inclusive) {
            set_cursor({cursor_.row, column});
            return true;
        }
        if (column == 0) {
            return false;
        }
        set_cursor({cursor_.row, column - 1});
        return true;
    }
    return false;
}

bool EditorCore::move_to_character_backward(char32_t target, bool inclusive) {
    if (cursor_.column == 0) {
        return false;
    }
    const std::u32string &line = lines_[cursor_.row];
    for (std::size_t column = cursor_.column; column-- > 0;) {
        if (line[column] != target) {
            continue;
        }
        set_cursor({cursor_.row, inclusive ? column : column + 1});
        return true;
    }
    return false;
}

void EditorCore::begin_selection(SelectionMode mode) {
    selection_anchor_ = cursor_;
    selection_mode_ = mode;
}

void EditorCore::clear_selection() {
    selection_anchor_.reset();
    selection_mode_ = SelectionMode::Character;
}

bool EditorCore::set_selection_range(Range range, SelectionMode mode) {
    Range normalized = normalized_range(range);
    if (positions_equal(normalized.start, normalized.end)) {
        return false;
    }

    if (mode == SelectionMode::Line) {
        selection_anchor_ = {normalized.start.row, 0};
        selection_mode_ = SelectionMode::Line;
        if (normalized.end.row > normalized.start.row && normalized.end.column == 0) {
            set_cursor({normalized.end.row - 1, 0});
        } else {
            set_cursor({normalized.end.row, 0});
        }
        return true;
    }

    selection_anchor_ = normalized.start;
    selection_mode_ = SelectionMode::Character;
    cursor_ = position_before(normalized.end);
    clamp_cursor();
    update_preferred_column();
    return true;
}

bool EditorCore::extend_selection_to_range(Range range) {
    Range normalized = normalized_range(range);
    if (positions_equal(normalized.start, normalized.end)) {
        return false;
    }
    if (!selection_anchor_) {
        return set_selection_range(normalized, SelectionMode::Character);
    }

    Range current = *selection_range();
    Range merged{
        position_less_than(current.start, normalized.start) ? current.start : normalized.start,
        position_less_than(current.end, normalized.end) ? normalized.end : current.end};
    return set_selection_range(merged, SelectionMode::Character);
}

void EditorCore::reset_history() {
    undo_stack_.clear();
    redo_stack_.clear();
    current_revision_ = 0;
    saved_revision_ = 0;
    document_version_ = 0;
    saved_document_version_ = 0;
}

void EditorCore::set_diagnostics(std::vector<Diagnostic> diagnostics) {
    set_document_diagnostics(document_uri_, std::move(diagnostics));
}

void EditorCore::clear_diagnostics() {
    clear_document_diagnostics(document_uri_);
}

void EditorCore::set_document_diagnostics(const std::string &document_uri, std::vector<Diagnostic> diagnostics) {
    diagnostics_by_uri_[document_uri] = std::move(diagnostics);
    if (document_uri == document_uri_) {
        emit_diagnostics_changed(document_uri);
        emit_annotations_changed(document_uri);
    }
}

void EditorCore::clear_document_diagnostics(const std::string &document_uri) {
    diagnostics_by_uri_.erase(document_uri);
    if (document_uri == document_uri_) {
        emit_diagnostics_changed(document_uri);
        emit_annotations_changed(document_uri);
    }
}

void EditorCore::set_annotations(std::vector<InlineAnnotation> annotations) {
    set_document_annotations(document_uri_, std::move(annotations));
}

void EditorCore::clear_annotations() {
    clear_document_annotations(document_uri_);
}

void EditorCore::set_document_annotations(const std::string &document_uri, std::vector<InlineAnnotation> annotations) {
    annotations_by_uri_[document_uri] = std::move(annotations);
    if (document_uri == document_uri_) {
        emit_annotations_changed(document_uri);
    }
}

void EditorCore::clear_document_annotations(const std::string &document_uri) {
    annotations_by_uri_.erase(document_uri);
    if (document_uri == document_uri_) {
        emit_annotations_changed(document_uri);
    }
}

bool EditorCore::load_file(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    std::string contents = buffer.str();

    std::vector<std::u32string> loaded_lines;
    std::string current_line;
    for (char ch : contents) {
        if (ch == '\n') {
            if (!current_line.empty() && current_line.back() == '\r') {
                current_line.pop_back();
            }
            loaded_lines.push_back(utf8_to_u32(current_line));
            current_line.clear();
            continue;
        }
        current_line.push_back(ch);
    }
    if (!current_line.empty() || contents.empty() || contents.back() != '\n') {
        if (!current_line.empty() && current_line.back() == '\r') {
            current_line.pop_back();
        }
        loaded_lines.push_back(utf8_to_u32(current_line));
    }

    std::string previous_uri = document_uri_;
    std::size_t previous_version = document_version_;

    lines_ = loaded_lines.empty() ? std::vector<std::u32string>{U""} : loaded_lines;
    file_path_ = path;
    document_uri_ = file_uri_for_path(path);
    cursor_ = {0, 0};
    selection_anchor_.reset();
    selection_mode_ = SelectionMode::Character;
    preferred_column_ = 0;
    reset_history();
    emit_document_closed(previous_uri, previous_version);
    emit_document_opened();
    return true;
}

bool EditorCore::save_file(const std::string &path) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    for (std::size_t row = 0; row < lines_.size(); ++row) {
        output << u32_to_utf8(lines_[row]);
        if (row + 1 < lines_.size()) {
            output << '\n';
        }
    }
    return true;
}

bool EditorCore::save_current_file() {
    if (!file_path_) {
        return false;
    }
    if (!save_file(*file_path_)) {
        return false;
    }
    saved_revision_ = current_revision_;
    saved_document_version_ = document_version_;
    emit_document_saved();
    return true;
}

bool EditorCore::save_current_file_as(const std::string &path) {
    std::string previous_uri = document_uri_;
    std::size_t previous_version = document_version_;
    if (!save_file(path)) {
        return false;
    }
    file_path_ = path;
    document_uri_ = file_uri_for_path(path);
    saved_revision_ = current_revision_;
    saved_document_version_ = document_version_;
    if (previous_uri != document_uri_) {
        emit_document_closed(previous_uri, previous_version);
        emit_document_opened();
    }
    emit_document_saved();
    return true;
}

std::u32string EditorCore::read_text(Range range) const {
    return read_range(range);
}

bool EditorCore::delete_range(Range range) {
    Range normalized = normalized_range(range);
    if (positions_equal(normalized.start, normalized.end)) {
        return false;
    }
    std::u32string deleted_text = read_range(normalized);
    apply_command(std::make_unique<ReplaceRangeCommand>(normalized, deleted_text, U""));
    clear_selection();
    return true;
}

bool EditorCore::replace_range(Range range, const std::u32string &text) {
    Range normalized = normalized_range(range);
    std::u32string replaced_text = read_range(normalized);
    if (positions_equal(normalized.start, normalized.end) && text.empty()) {
        return false;
    }
    apply_command(std::make_unique<ReplaceRangeCommand>(normalized, replaced_text, text));
    clear_selection();
    return true;
}

bool EditorCore::insert_text(Position position, const std::u32string &text) {
    return replace_range({position, position}, text);
}

bool EditorCore::apply_text_edits(const std::vector<TextEdit> &edits) {
    if (edits.empty()) {
        return false;
    }

    std::vector<TextEdit> sorted_edits = edits;
    std::sort(sorted_edits.begin(), sorted_edits.end(), [](const TextEdit &left, const TextEdit &right) {
        Range normalized_left = normalized_range(left.range);
        Range normalized_right = normalized_range(right.range);
        if (positions_equal(normalized_left.start, normalized_right.start)) {
            return position_less_than(normalized_right.end, normalized_left.end);
        }
        return position_less_than(normalized_right.start, normalized_left.start);
    });

    for (std::size_t index = 1; index < sorted_edits.size(); ++index) {
        if (ranges_overlap(sorted_edits[index - 1].range, sorted_edits[index].range)) {
            return false;
        }
    }

    std::vector<ReplaceRangeCommand> commands;
    commands.reserve(sorted_edits.size());
    for (const TextEdit &edit : sorted_edits) {
        Range normalized = normalized_range(edit.range);
        commands.emplace_back(normalized, read_range(normalized), edit.text);
    }

    apply_command(std::make_unique<TransactionCommand>(std::move(commands)));
    clear_selection();
    return true;
}

std::size_t EditorCore::utf8_offset_for_position(Position position) const {
    Position target = clamped_position(position);
    std::size_t offset = 0;
    for (std::size_t row = 0; row < target.row; ++row) {
        offset += u32_to_utf8(lines_[row]).size();
        offset += 1;
    }

    std::u32string prefix = lines_[target.row].substr(0, target.column);
    offset += u32_to_utf8(prefix).size();
    return offset;
}

Position EditorCore::position_for_utf8_offset(std::size_t offset) const {
    if (lines_.empty()) {
        return {0, 0};
    }

    std::size_t remaining = offset;
    for (std::size_t row = 0; row < lines_.size(); ++row) {
        const std::u32string &line = lines_[row];
        for (std::size_t column = 0; column < line.size(); ++column) {
            std::size_t width = u32_to_utf8(std::u32string(1, line[column])).size();
            if (remaining < width) {
                return {row, column};
            }
            remaining -= width;
        }

        if (row + 1 < lines_.size()) {
            if (remaining == 0) {
                return {row, line.size()};
            }
            if (remaining == 1) {
                return {row + 1, 0};
            }
            --remaining;
        }
    }

    return {lines_.size() - 1, lines_.back().size()};
}

Utf16Position EditorCore::utf16_position_for_position(Position position) const {
    Position target = clamped_position(position);
    std::size_t utf16_column = 0;
    for (std::size_t column = 0; column < target.column; ++column) {
        utf16_column += utf16_units_for_codepoint(lines_[target.row][column]);
    }
    return {target.row, utf16_column};
}

Position EditorCore::position_for_utf16(Utf16Position position) const {
    if (lines_.empty()) {
        return {0, 0};
    }

    if (position.row >= lines_.size()) {
        return {lines_.size() - 1, lines_.back().size()};
    }

    std::size_t utf16_column = 0;
    const std::u32string &line = lines_[position.row];
    for (std::size_t column = 0; column < line.size(); ++column) {
        std::size_t width = utf16_units_for_codepoint(line[column]);
        if (position.column < utf16_column + width) {
            return {position.row, column};
        }
        utf16_column += width;
    }
    return {position.row, line.size()};
}

std::optional<Range> EditorCore::inner_word_range() const {
    if (lines_.empty()) {
        return std::nullopt;
    }
    const std::u32string &line = lines_[cursor_.row];
    if (line.empty()) {
        return std::nullopt;
    }

    std::size_t index = cursor_.column >= line.size() ? line.size() - 1 : cursor_.column;
    if (is_space_codepoint(line[index])) {
        std::size_t next = index;
        while (next < line.size() && is_space_codepoint(line[next])) {
            ++next;
        }
        if (next < line.size()) {
            index = next;
        } else {
            std::size_t previous = index;
            while (previous > 0 && is_space_codepoint(line[previous])) {
                --previous;
            }
            if (is_space_codepoint(line[previous])) {
                return std::nullopt;
            }
            index = previous;
        }
    }

    bool word_class = is_word_codepoint(line[index]);
    bool punctuation_class = !word_class && !is_space_codepoint(line[index]);
    std::size_t start = index;
    while (start > 0) {
        char32_t codepoint = line[start - 1];
        if (word_class && is_word_codepoint(codepoint)) {
            --start;
            continue;
        }
        if (punctuation_class && !is_word_codepoint(codepoint) && !is_space_codepoint(codepoint)) {
            --start;
            continue;
        }
        break;
    }

    std::size_t end = index + 1;
    while (end < line.size()) {
        char32_t codepoint = line[end];
        if (word_class && is_word_codepoint(codepoint)) {
            ++end;
            continue;
        }
        if (punctuation_class && !is_word_codepoint(codepoint) && !is_space_codepoint(codepoint)) {
            ++end;
            continue;
        }
        break;
    }

    return Range{{cursor_.row, start}, {cursor_.row, end}};
}

std::optional<Range> EditorCore::a_word_range() const {
    std::optional<Range> inner = inner_word_range();
    if (!inner) {
        return std::nullopt;
    }

    Range range = *inner;
    const std::u32string &line = lines_[range.start.row];
    std::size_t end = range.end.column;
    while (end < line.size() && is_space_codepoint(line[end])) {
        ++end;
    }
    if (end > range.end.column) {
        range.end.column = end;
        return range;
    }

    std::size_t start = range.start.column;
    while (start > 0 && is_space_codepoint(line[start - 1])) {
        --start;
    }
    range.start.column = start;
    return range;
}

void EditorCore::apply_command(std::unique_ptr<EditCommand> command) {
    if (compound_depth_ > 0) {
        suppress_cursor_events_ = true;
        command->apply(*this);
        suppress_cursor_events_ = false;
        compound_commands_.push_back(std::move(command));
        redo_stack_.clear();
        return;
    }

    std::vector<std::u32string> before_lines = lines_;
    Position previous_cursor = cursor_;
    suppress_cursor_events_ = true;
    command->apply(*this);
    suppress_cursor_events_ = false;
    undo_stack_.push_back(std::move(command));
    redo_stack_.clear();
    ++current_revision_;
    ++document_version_;
    emit_document_changed(before_lines);
    emit_cursor_moved(previous_cursor);
}

void EditorCore::begin_compound_edit() {
    if (compound_depth_ == 0) {
        compound_before_lines_ = lines_;
        compound_before_cursor_ = cursor_;
        compound_commands_.clear();
    }
    ++compound_depth_;
}

void EditorCore::end_compound_edit() {
    if (compound_depth_ == 0) {
        return;
    }

    --compound_depth_;
    if (compound_depth_ > 0) {
        return;
    }

    if (compound_commands_.empty()) {
        return;
    }

    undo_stack_.push_back(std::make_unique<CompoundCommand>(std::move(compound_commands_)));
    compound_commands_.clear();
    redo_stack_.clear();
    ++current_revision_;
    ++document_version_;
    emit_document_changed(compound_before_lines_);
    emit_cursor_moved(compound_before_cursor_);
}

void EditorCore::raw_insert_codepoint(Position position, char32_t codepoint) {
    lines_[position.row].insert(position.column, 1, codepoint);
    set_cursor({position.row, position.column + 1});
}

void EditorCore::raw_remove_codepoint(Position position) {
    lines_[position.row].erase(position.column, 1);
    set_cursor(position);
}

void EditorCore::raw_split_line(Position position) {
    std::u32string tail = lines_[position.row].substr(position.column);
    lines_[position.row].erase(position.column);
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(position.row + 1), tail);
    set_cursor({position.row + 1, 0});
}

void EditorCore::raw_join_with_previous(std::size_t row) {
    std::size_t previous_length = lines_[row - 1].size();
    lines_[row - 1] += lines_[row];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(row));
    set_cursor({row - 1, previous_length});
}

void EditorCore::raw_join_with_next(std::size_t row) {
    lines_[row] += lines_[row + 1];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(row + 1));
}

void EditorCore::raw_insert_line(std::size_t row, const std::u32string &line) {
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(row), line);
    set_cursor({row, 0});
}

std::u32string EditorCore::raw_remove_line(std::size_t row) {
    std::u32string removed = lines_[row];
    if (lines_.size() == 1) {
        lines_[0].clear();
        set_cursor({0, 0});
        return removed;
    }
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(row));
    std::size_t new_row = row >= lines_.size() ? lines_.size() - 1 : row;
    set_cursor({new_row, cursor_.column});
    return removed;
}

void EditorCore::raw_insert_text(Position position, const std::u32string &text) {
    set_cursor(position);
    for (char32_t codepoint : text) {
        if (codepoint == U'\n') {
            raw_split_line(cursor_);
            continue;
        }
        raw_insert_codepoint(cursor_, codepoint);
    }
    set_cursor(position);
}

std::u32string EditorCore::read_range(Range range) const {
    Range normalized = normalized_range(range);
    if (positions_equal(normalized.start, normalized.end)) {
        return U"";
    }

    if (normalized.start.row == normalized.end.row) {
        return lines_[normalized.start.row].substr(
            normalized.start.column,
            normalized.end.column - normalized.start.column);
    }

    std::u32string text;
    text += lines_[normalized.start.row].substr(normalized.start.column);
    text.push_back(U'\n');
    for (std::size_t row = normalized.start.row + 1; row < normalized.end.row; ++row) {
        text += lines_[row];
        text.push_back(U'\n');
    }
    text += lines_[normalized.end.row].substr(0, normalized.end.column);
    return text;
}

Position EditorCore::position_after_text(Position position, const std::u32string &text) const {
    Position result = position;
    for (char32_t codepoint : text) {
        if (codepoint == U'\n') {
            ++result.row;
            result.column = 0;
        } else {
            ++result.column;
        }
    }
    return result;
}

std::u32string EditorCore::raw_delete_range(Range range) {
    Range normalized = normalized_range(range);
    std::u32string deleted_text = read_range(normalized);
    if (positions_equal(normalized.start, normalized.end)) {
        return deleted_text;
    }

    if (normalized.start.row == normalized.end.row) {
        lines_[normalized.start.row].erase(
            normalized.start.column,
            normalized.end.column - normalized.start.column);
        set_cursor(normalized.start);
        return deleted_text;
    }

    std::u32string prefix = lines_[normalized.start.row].substr(0, normalized.start.column);
    std::u32string suffix = lines_[normalized.end.row].substr(normalized.end.column);
    lines_[normalized.start.row] = prefix + suffix;
    lines_.erase(
        lines_.begin() + static_cast<std::ptrdiff_t>(normalized.start.row + 1),
        lines_.begin() + static_cast<std::ptrdiff_t>(normalized.end.row + 1));
    set_cursor(normalized.start);
    return deleted_text;
}

void EditorCore::insert_codepoint(char32_t codepoint) {
    apply_command(std::make_unique<InsertCodepointCommand>(cursor_, codepoint));
}

void EditorCore::insert_newline() {
    apply_command(std::make_unique<SplitLineCommand>(cursor_));
}

void EditorCore::backspace_character() {
    if (cursor_.column > 0) {
        Position deletion_position{cursor_.row, cursor_.column - 1};
        apply_command(std::make_unique<BackspaceCommand>(deletion_position, lines_[cursor_.row][cursor_.column - 1]));
        return;
    }
    if (cursor_.row > 0) {
        apply_command(std::make_unique<BackspaceCommand>(cursor_, lines_[cursor_.row]));
    }
}

void EditorCore::delete_character_under_cursor() {
    if (cursor_.column < lines_[cursor_.row].size()) {
        apply_command(std::make_unique<DeleteCharCommand>(cursor_, lines_[cursor_.row][cursor_.column]));
        return;
    }
    if (cursor_.row + 1 < lines_.size()) {
        apply_command(std::make_unique<DeleteCharCommand>(cursor_));
    }
}

void EditorCore::delete_current_line() {
    apply_command(std::make_unique<DeleteLineCommand>(
        cursor_.row,
        cursor_.column,
        lines_[cursor_.row],
        lines_.size() == 1));
}

void EditorCore::open_line_below() {
    apply_command(std::make_unique<InsertLineCommand>(cursor_.row + 1));
    set_cursor({cursor_.row, 0});
}

void EditorCore::open_line_above() {
    apply_command(std::make_unique<InsertLineCommand>(cursor_.row));
    set_cursor({cursor_.row, 0});
}

bool EditorCore::yank_selection() {
    std::optional<Range> selected = selection_range();
    if (!selected || positions_equal(selected->start, selected->end)) {
        return false;
    }
    yank_buffer_ = read_range(*selected);
    yank_mode_ = selection_mode_;
    return true;
}

bool EditorCore::delete_selection() {
    std::optional<Range> selected = selection_range();
    if (!selected || positions_equal(selected->start, selected->end)) {
        return false;
    }
    Range range = *selected;
    std::u32string deleted_text = read_range(range);
    yank_buffer_ = deleted_text;
    yank_mode_ = selection_mode_;
    apply_command(std::make_unique<ReplaceRangeCommand>(range, deleted_text, U""));
    clear_selection();
    return true;
}

bool EditorCore::paste_before_cursor() {
    if (yank_buffer_.empty()) {
        return false;
    }
    if (yank_mode_ == SelectionMode::Line) {
        return paste_linewise(true);
    }
    Range empty_range{cursor_, cursor_};
    apply_command(std::make_unique<ReplaceRangeCommand>(empty_range, U"", yank_buffer_));
    return true;
}

bool EditorCore::paste_after_cursor() {
    if (yank_buffer_.empty()) {
        return false;
    }
    if (yank_mode_ == SelectionMode::Line) {
        return paste_linewise(false);
    }
    Position insert_at = cursor_;
    std::size_t line_len = line_length(insert_at.row);
    if (insert_at.column < line_len) {
        ++insert_at.column;
    }
    Range empty_range{insert_at, insert_at};
    apply_command(std::make_unique<ReplaceRangeCommand>(empty_range, U"", yank_buffer_));
    return true;
}

bool EditorCore::replace_selection_with_yank() {
    if (yank_buffer_.empty()) {
        return false;
    }
    std::optional<Range> selected = selection_range();
    if (!selected || positions_equal(selected->start, selected->end)) {
        return false;
    }
    Range range = *selected;
    std::u32string replaced_text = read_range(range);
    apply_command(std::make_unique<ReplaceRangeCommand>(range, replaced_text, yank_buffer_));
    clear_selection();
    return true;
}

bool EditorCore::paste_linewise(bool before_cursor) {
    std::u32string text = yank_buffer_;
    if (ends_with_newline(text)) {
        text.pop_back();
    }

    Position insert_at;
    std::u32string inserted_text;
    if (before_cursor) {
        insert_at = {cursor_.row, 0};
        inserted_text = text;
        if (cursor_.row < lines_.size()) {
            inserted_text.push_back(U'\n');
        }
    } else if (cursor_.row + 1 < lines_.size()) {
        insert_at = {cursor_.row + 1, 0};
        inserted_text = text;
        inserted_text.push_back(U'\n');
    } else {
        insert_at = {cursor_.row, line_length(cursor_.row)};
        inserted_text.push_back(U'\n');
        inserted_text += text;
    }

    Range empty_range{insert_at, insert_at};
    apply_command(std::make_unique<ReplaceRangeCommand>(empty_range, U"", inserted_text));
    return true;
}

bool EditorCore::undo() {
    if (undo_stack_.empty()) {
        return false;
    }
    std::vector<std::u32string> before_lines = lines_;
    Position previous_cursor = cursor_;
    std::unique_ptr<EditCommand> command = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    suppress_cursor_events_ = true;
    command->undo(*this);
    suppress_cursor_events_ = false;
    redo_stack_.push_back(std::move(command));
    if (current_revision_ > 0) {
        --current_revision_;
    }
    ++document_version_;
    emit_document_changed(before_lines);
    emit_cursor_moved(previous_cursor);
    return true;
}

bool EditorCore::redo() {
    if (redo_stack_.empty()) {
        return false;
    }
    std::vector<std::u32string> before_lines = lines_;
    Position previous_cursor = cursor_;
    std::unique_ptr<EditCommand> command = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    suppress_cursor_events_ = true;
    command->apply(*this);
    suppress_cursor_events_ = false;
    undo_stack_.push_back(std::move(command));
    ++current_revision_;
    ++document_version_;
    emit_document_changed(before_lines);
    emit_cursor_moved(previous_cursor);
    return true;
}
