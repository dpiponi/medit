#include "config.hpp"
#include "editor_commands.hpp"
#include "editor_core.hpp"
#include "editor_session.hpp"
#include "keybindings.hpp"
#include "lsp_service.hpp"
#include "process_utils.hpp"
#include "services.hpp"
#include "syntax.hpp"
#include "string_utils.hpp"
#include "theme.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <curses.h>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

std::string buffer_text(const EditorCore &core) {
    std::string text;
    const auto &lines = core.lines();
    for (std::size_t row = 0; row < lines.size(); ++row) {
        text += u32_to_utf8(lines[row]);
        if (row + 1 < lines.size()) {
            text += '\n';
        }
    }
    return text;
}

void expect(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_text(const EditorCore &core, const std::string &expected, const std::string &message) {
    expect(buffer_text(core) == expected, message + ": expected [" + expected + "] got [" + buffer_text(core) + "]");
}

void expect_cursor(const EditorCore &core, Position expected, const std::string &message) {
    Position actual = core.cursor();
    expect(
        actual.row == expected.row && actual.column == expected.column,
        message + ": expected cursor (" + std::to_string(expected.row) + "," + std::to_string(expected.column) +
            ") got (" + std::to_string(actual.row) + "," + std::to_string(actual.column) + ")");
}

void expect_event_type(const EditorEvent &event, EditorEventType expected, const std::string &message) {
    expect(event.type == expected, message);
}

bool line_has_span(
    const std::vector<HighlightSpan> &spans,
    std::size_t start,
    std::size_t end,
    StyleRole role) {
    for (const HighlightSpan &span : spans) {
        if (span.role == role && span.range.start.column == start && span.range.end.column == end) {
            return true;
        }
    }
    return false;
}

void test_insert_unicode_and_undo() {
    EditorCore core;
    core.insert_codepoint(U'a');
    core.insert_codepoint(U'B');
    core.insert_codepoint(U'\u03c0');
    expect_text(core, "aB\xCF\x80", "unicode insert");
    expect(core.undo(), "undo should succeed");
    expect_text(core, "aB", "undo removed unicode codepoint");
    expect(core.redo(), "redo should succeed");
    expect_text(core, "aB\xCF\x80", "redo restored unicode codepoint");
}

void test_newline_backspace_and_join() {
    EditorCore core;
    core.insert_codepoint(U'a');
    core.insert_codepoint(U'b');
    core.insert_newline();
    core.insert_codepoint(U'c');
    expect_text(core, "ab\nc", "newline split");
    core.backspace_character();
    expect_text(core, "ab\n", "backspace removed character");
    core.backspace_character();
    expect_text(core, "ab", "backspace joined lines");
}

void test_insert_mode_soft_tab_and_shift_tab() {
    EditorCore core;
    expect(core.insert_soft_tab(4), "soft tab at line start should succeed");
    expect_text(core, "    ", "soft tab inserts spaces to next shiftwidth stop");
    expect(core.insert_soft_tab(4), "second soft tab should succeed");
    expect_text(core, "        ", "soft tab advances to the next shiftwidth stop");
    expect(core.outdent_before_cursor(4), "shift-tab style outdent should succeed");
    expect_text(core, "    ", "shift-tab outdents to previous shiftwidth stop");
    expect(core.insert_text({0, 4}, utf8_to_u32("x")), "insert non-whitespace text");
    core.set_cursor({0, 5});
    expect(core.insert_soft_tab(4), "soft tab outside indentation should insert a tab");
    expect_text(core, "    x\t", "soft tab outside leading whitespace inserts a tab");
}

void test_visual_range_semantics_and_delete() {
    EditorCore core;
    for (char ch : std::string("abcd")) {
        core.insert_codepoint(static_cast<char32_t>(ch));
    }
    core.set_cursor({0, 2});
    core.begin_selection();
    core.move_left();
    std::optional<Range> range = core.selection_range();
    expect(range.has_value(), "selection range should exist");
    expect(range->start.row == 0 && range->start.column == 1, "backward selection start");
    expect(range->end.row == 0 && range->end.column == 3, "backward selection end includes anchor char");
    expect(core.delete_selection(), "delete selection should succeed");
    expect_text(core, "ad", "delete selection removed selected range");
}

void test_linewise_selection_range_and_delete() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("one\ntwo\nthree\nfour")), "seed linewise selection buffer");

    core.set_cursor({1, 1});
    core.begin_selection(SelectionMode::Line);
    core.move_down();

    std::optional<Range> range = core.selection_range();
    expect(range.has_value(), "linewise selection range should exist");
    expect(range->start.row == 1 && range->start.column == 0, "linewise selection starts at line boundary");
    expect(range->end.row == 3 && range->end.column == 0, "linewise selection ends at next line start");
    expect(u32_to_utf8(core.read_text(*range)) == "two\nthree\n", "linewise selection reads full lines");

    expect(core.delete_selection(), "linewise delete should succeed");
    expect_text(core, "one\nfour", "linewise delete removes complete selected lines");
    expect(core.undo(), "undo linewise delete");
    expect_text(core, "one\ntwo\nthree\nfour", "undo linewise delete restores lines");
}

void test_linewise_delete_and_paste() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("one\ntwo\nthree")), "seed linewise paste buffer");

    core.set_cursor({1, 0});
    core.begin_selection(SelectionMode::Line);
    expect(core.delete_selection(), "linewise delete of one line should succeed");
    expect(core.yank_mode() == SelectionMode::Line, "linewise delete should set linewise yank mode");
    expect_text(core, "one\nthree", "linewise delete removes the selected line");

    expect(core.paste_after_cursor(), "linewise paste after should succeed");
    expect_text(core, "one\nthree\ntwo", "linewise paste after inserts full line below cursor");
    expect(core.undo(), "undo linewise paste after");
    expect_text(core, "one\nthree", "undo linewise paste after restores buffer");

    expect(core.paste_before_cursor(), "linewise paste before should succeed");
    expect_text(core, "one\ntwo\nthree", "linewise paste before inserts full line above cursor");
}

void test_yank_and_paste() {
    EditorCore core;
    for (char ch : std::string("abcd")) {
        core.insert_codepoint(static_cast<char32_t>(ch));
    }
    core.set_cursor({0, 0});
    core.begin_selection();
    core.move_right();
    expect(core.yank_selection(), "yank selection should succeed");
    expect(u32_to_utf8(core.yank_buffer()) == "ab", "yank buffer contents");
    core.clear_selection();
    core.set_cursor({0, 3});
    expect(core.paste_before_cursor(), "paste before should succeed");
    expect_text(core, "abcabd", "paste before cursor");
    expect(core.undo(), "undo paste before");
    expect_text(core, "abcd", "undo paste before restored");
    expect(core.paste_after_cursor(), "paste after should succeed");
    expect_text(core, "abcdab", "paste after cursor");
}

void test_indent_and_outdent_lines() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("one\ntwo\n\tthree")), "seed indent buffer");
    expect(core.indent_lines(0, 1, 2), "indent first two lines");
    expect_text(core, "  one\n  two\n\tthree", "indent adds spaces to selected lines");
    expect(core.undo(), "undo indent");
    expect_text(core, "one\ntwo\n\tthree", "undo indent restores text");

    expect(core.outdent_lines(2, 2, 2), "outdent tab-indented line");
    expect_text(core, "one\ntwo\nthree", "outdent removes a leading tab");
    expect(core.undo(), "undo outdent");
    expect_text(core, "one\ntwo\n\tthree", "undo outdent restores text");
}

void test_replace_selection_with_yank() {
    EditorCore core;
    for (char ch : std::string("abcd")) {
        core.insert_codepoint(static_cast<char32_t>(ch));
    }
    core.set_cursor({0, 0});
    core.begin_selection();
    core.move_right();
    expect(core.yank_selection(), "yank source selection");
    core.clear_selection();
    core.set_cursor({0, 2});
    core.begin_selection();
    core.move_right();
    expect(core.replace_selection_with_yank(), "replace selection should succeed");
    expect_text(core, "abab", "replace selection with yank buffer");
    expect(core.undo(), "undo replace selection");
    expect_text(core, "abcd", "undo replace selection restored");
}

void test_file_io_and_dirty_tracking() {
    EditorCore core;
    core.insert_codepoint(U'h');
    core.insert_codepoint(U'i');
    expect(core.document_version() == 2, "document version should advance with edits");

    char path[] = "/tmp/medit-test-XXXXXX";
    int fd = mkstemp(path);
    expect(fd >= 0, "mkstemp should succeed");
    close(fd);

    expect(core.save_current_file_as(path), "save as should succeed");
    expect(!core.is_dirty(), "save clears dirty state");
    expect(core.saved_document_version() == core.document_version(), "save syncs saved document version");

    EditorCore reopened;
    expect(reopened.load_file(path), "load file should succeed");
    expect_text(reopened, "hi", "reloaded file contents");
    expect(reopened.document_version() == 0, "load resets document version");

    reopened.insert_newline();
    reopened.insert_codepoint(U'!');
    expect(reopened.is_dirty(), "editing after load marks dirty");
    expect(reopened.document_version() == 2, "loaded buffer document version advances from zero");
    expect(reopened.save_current_file(), "save current file should succeed");
    expect(!reopened.is_dirty(), "save current clears dirty state");
    expect(
        reopened.saved_document_version() == reopened.document_version(),
        "save current syncs saved document version");

    std::remove(path);
}

void test_open_empty_missing_file_and_save() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_missing_file_core";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::filesystem::path path = root / "new-file.txt";

    EditorCore core;
    core.open_empty_file(path.string());
    expect(core.file_path().has_value() && *core.file_path() == path.string(), "empty file should keep requested path");
    expect(core.document_uri() == file_uri_for_path(path.string()), "empty file should use requested document uri");
    expect_text(core, "", "empty file should start empty");
    expect(!core.is_dirty(), "new empty named buffer should start clean");

    core.insert_codepoint(U'x');
    expect(core.save_current_file(), "save_current_file should create missing file");

    EditorCore reopened;
    expect(reopened.load_file(path.string()), "reopened created file should load");
    expect_text(reopened, "x", "created file should contain saved text");

    std::filesystem::remove_all(root);
}

void test_navigation() {
    EditorCore core;
    for (char ch : std::string("a")) {
        core.insert_codepoint(static_cast<char32_t>(ch));
    }
    core.insert_newline();
    core.insert_codepoint(U'b');
    core.insert_newline();
    core.insert_codepoint(U'c');
    core.move_to_first_line();
    expect_cursor(core, {0, 1}, "move_to_first_line preserves preferred column");
    core.move_to_last_line();
    expect_cursor(core, {2, 1}, "move_to_last_line preserves preferred column");
    core.move_by_lines(-1);
    expect_cursor(core, {1, 1}, "move_by_lines up preserves preferred column");
    core.move_line_start();
    expect_cursor(core, {1, 0}, "move_line_start");
}

void test_find_and_till_character_motions() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("abc def ghi")), "seed find motion buffer");

    core.set_cursor({0, 0});
    expect(core.move_to_character_forward(U'd', true), "find forward should succeed");
    expect_cursor(core, {0, 4}, "find forward lands on target");

    core.set_cursor({0, 0});
    expect(core.move_to_character_forward(U'd', false), "till forward should succeed");
    expect_cursor(core, {0, 3}, "till forward lands before target");

    core.set_cursor({0, 8});
    expect(core.move_to_character_backward(U'd', true), "find backward should succeed");
    expect_cursor(core, {0, 4}, "find backward lands on target");

    core.set_cursor({0, 8});
    expect(core.move_to_character_backward(U'd', false), "till backward should succeed");
    expect_cursor(core, {0, 5}, "till backward lands after target");
}

void test_word_object_ranges() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("  alpha, beta  ")), "seed word object buffer");

    core.set_cursor({0, 3});
    std::optional<Range> inner = core.inner_word_range();
    expect(inner.has_value(), "inner word range should exist");
    expect(inner->start.column == 2 && inner->end.column == 7, "inner word selects bare word");

    core.set_cursor({0, 9});
    std::optional<Range> around = core.a_word_range();
    expect(around.has_value(), "around word range should exist");
    expect(around->start.column == 9 && around->end.column == 15, "around word includes trailing spaces");

    core.set_cursor({0, 7});
    std::optional<Range> punctuation = core.inner_word_range();
    expect(punctuation.has_value(), "punctuation object should exist");
    expect(
        punctuation->start.column == 7 && punctuation->end.column == 8,
        "inner word on punctuation selects punctuation run");
}

void test_extend_selection_to_word_object() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("alpha beta")), "seed selection extension buffer");

    core.set_cursor({0, 0});
    core.begin_selection(SelectionMode::Character);
    core.move_right();
    std::optional<Range> around = core.a_word_range();
    expect(around.has_value(), "around word range should exist for extension");
    expect(core.extend_selection_to_range(*around), "selection should extend to around-word range");

    std::optional<Range> selection = core.selection_range();
    expect(selection.has_value(), "extended selection should exist");
    expect(selection->start.column == 0 && selection->end.column == 6, "selection extends to include whole first word");
}

void test_generic_range_edit_api() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("alpha\nbeta\ngamma")), "insert_text should succeed");
    expect_text(core, "alpha\nbeta\ngamma", "insert_text populates buffer");

    std::u32string selected = core.read_text({{2, 2}, {0, 2}});
    expect(u32_to_utf8(selected) == "pha\nbeta\nga", "read_text normalizes backward ranges");

    expect(core.replace_range({{0, 1}, {1, 2}}, utf8_to_u32("X\nY")), "replace_range should succeed");
    expect_text(core, "aX\nYta\ngamma", "replace_range updates multiple lines");
    expect(core.undo(), "undo replace_range");
    expect_text(core, "alpha\nbeta\ngamma", "undo replace_range restores text");
    expect(core.redo(), "redo replace_range");
    expect_text(core, "aX\nYta\ngamma", "redo replace_range restores replacement");

    expect(core.delete_range({{0, 1}, {1, 1}}), "delete_range should succeed");
    expect_text(core, "ata\ngamma", "delete_range removes selected text");
    expect(core.undo(), "undo delete_range");
    expect_text(core, "aX\nYta\ngamma", "undo delete_range restores text");

    expect(core.insert_text({2, 5}, utf8_to_u32("!")), "insert_text appends text");
    expect_text(core, "aX\nYta\ngamma!", "insert_text appends at arbitrary position");
}

void test_text_edit_transactions() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("abcd\nef")), "seed buffer with insert_text");
    std::size_t base_revision = core.current_revision();

    std::vector<TextEdit> edits = {
        {{{0, 1}, {0, 3}}, utf8_to_u32("XX")},
        {{{1, 2}, {1, 2}}, utf8_to_u32("!")},
        {{{0, 0}, {0, 1}}, U""},
    };
    expect(core.apply_text_edits(edits), "apply_text_edits should succeed");
    expect_text(core, "XXd\nef!", "apply_text_edits applies original-coordinate edits atomically");
    expect(core.current_revision() == base_revision + 1, "transaction should consume one revision");

    expect(core.undo(), "undo text edit transaction");
    expect_text(core, "abcd\nef", "undo transaction restores original buffer");
    expect(core.current_revision() == base_revision, "undo transaction restores revision");

    expect(core.redo(), "redo text edit transaction");
    expect_text(core, "XXd\nef!", "redo transaction reapplies all edits");
}

void test_text_edit_transaction_rejects_overlaps() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("abcd")), "seed overlap test buffer");
    std::size_t base_revision = core.current_revision();

    std::vector<TextEdit> overlapping = {
        {{{0, 1}, {0, 3}}, utf8_to_u32("X")},
        {{{0, 2}, {0, 4}}, utf8_to_u32("Y")},
    };
    expect(!core.apply_text_edits(overlapping), "overlapping edits should be rejected");
    expect_text(core, "abcd", "rejected overlapping edits leave buffer unchanged");
    expect(core.current_revision() == base_revision, "rejected overlapping edits do not bump revision");
}

void test_document_version_is_monotonic() {
    EditorCore core;
    expect(core.document_version() == 0, "new buffer starts at document version zero");

    core.insert_codepoint(U'a');
    expect(core.document_version() == 1, "insert increments document version");
    expect(core.undo(), "undo should succeed for document version test");
    expect(core.document_version() == 2, "undo increments document version");
    expect(core.redo(), "redo should succeed for document version test");
    expect(core.document_version() == 3, "redo increments document version");
}

void test_document_identity_and_events() {
    EditorCore core;
    expect(core.document_uri().starts_with("untitled://medit/"), "new buffer has untitled document uri");
    expect(core.pending_events().empty(), "new buffer starts with no queued events");

    core.insert_codepoint(U'a');
    std::vector<EditorEvent> edit_events = core.take_events();
    expect(edit_events.size() == 2, "edit should emit change and cursor events");
    expect_event_type(edit_events[0], EditorEventType::DocumentChanged, "edit emits document changed first");
    expect(edit_events[0].document_uri == core.document_uri(), "changed event uses current document uri");
    expect(edit_events[0].document_version == core.document_version(), "changed event carries current version");
    expect(edit_events[0].range.has_value(), "changed event carries replaced range");
    expect(u32_to_utf8(edit_events[0].text) == "a", "changed event carries new buffer text");
    expect_event_type(edit_events[1], EditorEventType::CursorMoved, "edit emits cursor moved");

    core.set_cursor({0, 0});
    std::vector<EditorEvent> move_events = core.take_events();
    expect(move_events.size() == 1, "explicit cursor set emits one cursor event");
    expect_event_type(move_events[0], EditorEventType::CursorMoved, "cursor set emits cursor moved");
}

void test_open_save_and_save_as_events() {
    char path[] = "/tmp/medit-open-XXXXXX";
    int fd = mkstemp(path);
    expect(fd >= 0, "mkstemp for open event test should succeed");
    close(fd);
    {
        std::ofstream file(path);
        file << "hello";
    }

    EditorCore core;
    std::string previous_uri = core.document_uri();
    expect(core.load_file(path), "load_file should succeed for event test");
    std::vector<EditorEvent> load_events = core.take_events();
    expect(load_events.size() == 2, "load should emit close and open events");
    expect_event_type(load_events[0], EditorEventType::DocumentClosed, "load emits close for previous document");
    expect(load_events[0].document_uri == previous_uri, "close event uses previous document uri");
    expect_event_type(load_events[1], EditorEventType::DocumentOpened, "load emits open for new document");
    expect(load_events[1].document_uri == core.document_uri(), "open event uses new document uri");
    expect(u32_to_utf8(load_events[1].text) == "hello", "open event includes loaded buffer text");

    expect(core.save_current_file(), "save_current_file should succeed for event test");
    std::vector<EditorEvent> save_events = core.take_events();
    expect(save_events.size() == 1, "save should emit one event");
    expect_event_type(save_events[0], EditorEventType::DocumentSaved, "save emits saved event");

    char path2[] = "/tmp/medit-save-as-XXXXXX";
    int fd2 = mkstemp(path2);
    expect(fd2 >= 0, "mkstemp for save-as event test should succeed");
    close(fd2);

    std::string loaded_uri = core.document_uri();
    expect(core.save_current_file_as(path2), "save_current_file_as should succeed");
    std::vector<EditorEvent> save_as_events = core.take_events();
    expect(save_as_events.size() == 3, "save as should emit close, open, and save events when uri changes");
    expect_event_type(save_as_events[0], EditorEventType::DocumentClosed, "save as closes previous document identity");
    expect(save_as_events[0].document_uri == loaded_uri, "save as close event uses old uri");
    expect_event_type(save_as_events[1], EditorEventType::DocumentOpened, "save as opens new document identity");
    expect(save_as_events[1].document_uri == core.document_uri(), "save as open event uses new uri");
    expect_event_type(save_as_events[2], EditorEventType::DocumentSaved, "save as emits saved event");

    std::remove(path);
    std::remove(path2);
}

void test_unicode_position_conversions() {
    EditorCore core;
    expect(
        core.insert_text({0, 0}, utf8_to_u32("A\xCF\x80\nx\xF0\x9F\x98\x80" "e\xCC\x81")),
        "seed unicode conversion buffer");

    expect(core.utf8_offset_for_position({0, 0}) == 0, "utf8 offset at buffer start");
    expect(core.utf8_offset_for_position({0, 1}) == 1, "ascii utf8 width");
    expect(core.utf8_offset_for_position({0, 2}) == 3, "pi utf8 width");
    expect(core.utf8_offset_for_position({1, 0}) == 4, "newline contributes one utf8 byte");
    expect(core.utf8_offset_for_position({1, 1}) == 5, "emoji starts after leading ascii on second line");
    expect(core.utf8_offset_for_position({1, 2}) == 9, "emoji contributes four utf8 bytes");
    expect(core.utf8_offset_for_position({1, 4}) == 12, "combining mark contributes two utf8 bytes");

    Position start = core.position_for_utf8_offset(0);
    expect(start.row == 0 && start.column == 0, "position for utf8 offset zero");
    expect(core.position_for_utf8_offset(3).row == 0 && core.position_for_utf8_offset(3).column == 2, "utf8 offset for pi boundary");
    expect(core.position_for_utf8_offset(4).row == 1 && core.position_for_utf8_offset(4).column == 0, "utf8 offset at second line start");
    expect(core.position_for_utf8_offset(8).row == 1 && core.position_for_utf8_offset(8).column == 1, "utf8 offset inside emoji clamps to emoji start");
    expect(core.position_for_utf8_offset(12).row == 1 && core.position_for_utf8_offset(12).column == 4, "utf8 offset at line end");

    Utf16Position pi = core.utf16_position_for_position({0, 2});
    expect(pi.row == 0 && pi.column == 2, "utf16 position for bmp codepoints");
    Utf16Position emoji = core.utf16_position_for_position({1, 2});
    expect(emoji.row == 1 && emoji.column == 3, "emoji counts as two utf16 units");
    Utf16Position combining = core.utf16_position_for_position({1, 4});
    expect(combining.row == 1 && combining.column == 5, "combining mark counts as one utf16 unit");

    Position from_utf16_emoji = core.position_for_utf16({1, 3});
    expect(from_utf16_emoji.row == 1 && from_utf16_emoji.column == 2, "utf16 roundtrip after emoji");
    Position inside_surrogate = core.position_for_utf16({1, 2});
    expect(inside_surrogate.row == 1 && inside_surrogate.column == 1, "utf16 position inside surrogate pair clamps to codepoint start");
    Position from_utf16_end = core.position_for_utf16({1, 5});
    expect(from_utf16_end.row == 1 && from_utf16_end.column == 4, "utf16 line end maps to codepoint end");
}

void test_diagnostics_storage_and_events() {
    EditorCore core;
    Diagnostic error{{{0, 0}, {0, 1}}, DiagnosticSeverity::Error, "test", utf8_to_u32("bad")};
    Diagnostic warning{{{0, 1}, {0, 2}}, DiagnosticSeverity::Warning, "lint", utf8_to_u32("warn")};

    core.set_diagnostics({error, warning});
    expect(core.diagnostics().size() == 2, "set_diagnostics should store current document diagnostics");
    expect(core.projected_annotations().size() == 2, "diagnostics should project to inline annotations");

    std::vector<EditorEvent> events = core.take_events();
    expect(events.size() == 2, "setting diagnostics should emit diagnostics and annotations events");
    expect_event_type(events[0], EditorEventType::DiagnosticsChanged, "diagnostics change should emit diagnostics event");
    expect_event_type(events[1], EditorEventType::AnnotationsChanged, "diagnostics change should emit annotations event");
    expect(events[0].document_uri == core.document_uri(), "diagnostics event should use current document uri");

    core.clear_diagnostics();
    expect(core.diagnostics().empty(), "clear_diagnostics should remove current diagnostics");
    events = core.take_events();
    expect(events.size() == 2, "clearing diagnostics should emit diagnostics and annotations events");
    expect_event_type(events[0], EditorEventType::DiagnosticsChanged, "clearing diagnostics emits diagnostics event");
    expect_event_type(events[1], EditorEventType::AnnotationsChanged, "clearing diagnostics emits annotations event");
}

void test_annotations_storage_and_events() {
    EditorCore core;
    InlineAnnotation note{{{0, 0}, {0, 1}}, AnnotationSeverity::Info, AnnotationKind::Note, "note", utf8_to_u32("hello\nworld")};
    core.set_annotations({note});
    expect(core.annotations().size() == 1, "set_annotations should store current document annotations");
    expect(core.projected_annotations().size() == 1, "projected annotations should include explicit annotations");

    std::vector<EditorEvent> events = core.take_events();
    expect(events.size() == 1, "setting annotations should emit one event");
    expect_event_type(events[0], EditorEventType::AnnotationsChanged, "setting annotations emits annotations changed");

    core.clear_annotations();
    expect(core.annotations().empty(), "clear_annotations should remove current document annotations");
    events = core.take_events();
    expect(events.size() == 1, "clearing annotations should emit one event");
    expect_event_type(events[0], EditorEventType::AnnotationsChanged, "clearing annotations emits annotations changed");
}

void test_diagnostics_follow_document_switches() {
    char path1[] = "/tmp/medit-diagnostics-a-XXXXXX";
    char path2[] = "/tmp/medit-diagnostics-b-XXXXXX";
    int fd1 = mkstemp(path1);
    int fd2 = mkstemp(path2);
    expect(fd1 >= 0 && fd2 >= 0, "mkstemp for diagnostics switch test should succeed");
    close(fd1);
    close(fd2);
    {
        std::ofstream file1(path1);
        file1 << "alpha";
    }
    {
        std::ofstream file2(path2);
        file2 << "beta";
    }

    EditorCore core;
    expect(core.load_file(path1), "load first file for diagnostics switch test");
    core.take_events();
    core.set_diagnostics({{{{0, 0}, {0, 5}}, DiagnosticSeverity::Error, "first", utf8_to_u32("alpha error")}});
    std::string uri1 = core.document_uri();
    core.take_events();

    expect(core.load_file(path2), "load second file for diagnostics switch test");
    expect(core.diagnostics().empty(), "newly loaded file should start with no diagnostics");
    core.take_events();
    core.set_diagnostics({{{{0, 0}, {0, 4}}, DiagnosticSeverity::Warning, "second", utf8_to_u32("beta warn")}});
    std::string uri2 = core.document_uri();
    core.take_events();

    expect(core.load_file(path1), "reload first file for diagnostics switch test");
    expect(core.document_uri() == uri1, "reloaded first file should restore first uri");
    expect(core.diagnostics().size() == 1, "reloaded first file should restore stored diagnostics");
    expect(core.diagnostics()[0].severity == DiagnosticSeverity::Error, "first file should restore first severity");

    expect(core.load_file(path2), "reload second file for diagnostics switch test");
    expect(core.document_uri() == uri2, "reloaded second file should restore second uri");
    expect(core.diagnostics().size() == 1, "reloaded second file should restore second diagnostics");
    expect(core.diagnostics()[0].severity == DiagnosticSeverity::Warning, "second file should restore second severity");

    std::remove(path1);
    std::remove(path2);
}

void test_editor_command_entry_points() {
    EditorCore core;

    EditorCommand set_diagnostics;
    set_diagnostics.type = EditorCommandType::SetDiagnostics;
    set_diagnostics.diagnostics = {
        {{{0, 0}, {0, 1}}, DiagnosticSeverity::Error, "cmd", utf8_to_u32("bad")}
    };
    EditorCommandResult result = apply_editor_command(core, set_diagnostics);
    expect(result.applied, "set diagnostics command should apply");
    expect(core.diagnostics().size() == 1, "set diagnostics command should update core diagnostics");

    EditorCommand move_cursor;
    move_cursor.type = EditorCommandType::MoveCursor;
    move_cursor.position = Position{0, 0};
    result = apply_editor_command(core, move_cursor);
    expect(result.applied, "move cursor command should apply");
    expect_cursor(core, {0, 0}, "move cursor command should update cursor");

    EditorCommand set_annotations;
    set_annotations.type = EditorCommandType::SetAnnotations;
    set_annotations.annotations = {
        {{{0, 0}, {0, 1}}, AnnotationSeverity::Info, AnnotationKind::Note, "cmd", utf8_to_u32("note")}
    };
    result = apply_editor_command(core, set_annotations);
    expect(result.applied, "set annotations command should apply");
    expect(core.annotations().size() == 1, "set annotations command should update core annotations");

    EditorCommand status;
    status.type = EditorCommandType::SetStatusMessage;
    status.message = "service says hi";
    result = apply_editor_command(core, status);
    expect(result.applied, "status command should apply");
    expect(result.status_message.has_value(), "status command should return status message");
    expect(*result.status_message == "service says hi", "status command should preserve message");
}

void test_compound_edit_undo() {
    EditorCore core;
    core.begin_compound_edit();
    core.insert_codepoint(U'a');
    core.insert_codepoint(U'b');
    core.insert_codepoint(U'c');
    core.end_compound_edit();

    expect_text(core, "abc", "compound edit should apply all edits");
    expect(core.undo(), "compound edit should undo");
    expect_text(core, "", "compound undo should revert entire sequence");
    expect(core.redo(), "compound edit should redo");
    expect_text(core, "abc", "compound redo should restore entire sequence");
}

void test_keybinding_dispatch() {
    KeyBindings keybindings = load_embedded_keybindings();
    std::vector<std::string> pending;

    KeyDispatch first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(first.matched && first.waiting_for_more && !first.action.has_value(), "g should wait for gg");

    KeyDispatch second = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(second.action.has_value() && *second.action == EditorAction::GotoTop, "gg should map to goto top");

    KeyDispatch printable = dispatch_key_sequence(keybindings, "insert", pending, "x", true);
    expect(printable.action.has_value() && *printable.action == EditorAction::SelfInsert, "insert printable binding");

    KeyDispatch insert_tab = dispatch_key_sequence(keybindings, "insert", pending, "tab", false);
    expect(insert_tab.action.has_value() && *insert_tab.action == EditorAction::InsertSoftTab, "insert tab binding");

    KeyDispatch insert_shift_tab = dispatch_key_sequence(keybindings, "insert", pending, "shift-tab", false);
    expect(
        insert_shift_tab.action.has_value() && *insert_shift_tab.action == EditorAction::InsertOutdent,
        "insert shift-tab binding");

    KeyDispatch special = dispatch_key_sequence(keybindings, "normal", pending, "pagedown", false);
    expect(special.action.has_value() && *special.action == EditorAction::PageDown, "pagedown binding");

    KeyDispatch indent = dispatch_key_sequence(keybindings, "normal", pending, ">", false);
    expect(indent.action.has_value() && *indent.action == EditorAction::Indent, "> binding");

    KeyDispatch outdent = dispatch_key_sequence(keybindings, "normal", pending, "<", false);
    expect(outdent.action.has_value() && *outdent.action == EditorAction::Outdent, "< binding");

    KeyDispatch next_buffer = dispatch_key_sequence(keybindings, "normal", pending, "tab", false);
    expect(next_buffer.action.has_value() && *next_buffer.action == EditorAction::NextBuffer, "tab binding");

    KeyDispatch previous_buffer = dispatch_key_sequence(keybindings, "normal", pending, "shift-tab", false);
    expect(
        previous_buffer.action.has_value() && *previous_buffer.action == EditorAction::PreviousBuffer,
        "shift-tab binding");

    KeyDispatch suspend = dispatch_key_sequence(keybindings, "normal", pending, "ctrl-z", false);
    expect(suspend.action.has_value() && *suspend.action == EditorAction::Suspend, "ctrl-z binding");

    KeyDispatch linewise = dispatch_key_sequence(keybindings, "normal", pending, "V", false);
    expect(
        linewise.action.has_value() && *linewise.action == EditorAction::EnterVisualLineMode,
        "V should map to linewise visual mode");

    KeyDispatch filter = dispatch_key_sequence(keybindings, "visual", pending, "|", false);
    expect(
        filter.action.has_value() && *filter.action == EditorAction::FilterSelection,
        "| should map to filter selection in visual mode");

    KeyDispatch sed = dispatch_key_sequence(keybindings, "visual", pending, "S", false);
    expect(
        sed.action.has_value() && *sed.action == EditorAction::SedSelection,
        "S should map to sed selection in visual mode");

    KeyDispatch command_history_previous = dispatch_key_sequence(keybindings, "command", pending, "up", false);
    expect(
        command_history_previous.action.has_value() &&
            *command_history_previous.action == EditorAction::CommandHistoryPrevious,
        "command up should browse previous history");

    KeyDispatch command_history_next = dispatch_key_sequence(keybindings, "command", pending, "down", false);
    expect(
        command_history_next.action.has_value() &&
            *command_history_next.action == EditorAction::CommandHistoryNext,
        "command down should browse next history");

    KeyDispatch find = dispatch_key_sequence(keybindings, "normal", pending, "f", false);
    expect(find.action.has_value() && *find.action == EditorAction::FindForward, "f should map to find forward");

    KeyDispatch search = dispatch_key_sequence(keybindings, "normal", pending, "/", false);
    expect(
        search.action.has_value() && *search.action == EditorAction::EnterSearchMode,
        "/ should map to search mode");

    KeyDispatch search_prev = dispatch_key_sequence(keybindings, "normal", pending, "b", false);
    expect(
        search_prev.action.has_value() && *search_prev.action == EditorAction::SearchPrevious,
        "b should map to previous search result");

    KeyDispatch definition_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(definition_first.matched && definition_first.waiting_for_more, "g should wait for gd");
    KeyDispatch definition_second = dispatch_key_sequence(keybindings, "normal", pending, "d", false);
    expect(
        definition_second.action.has_value() && *definition_second.action == EditorAction::GoToDefinition,
        "gd should map to go to definition");

    KeyDispatch file_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(file_first.matched && file_first.waiting_for_more, "g should wait for gf");
    KeyDispatch file_second = dispatch_key_sequence(keybindings, "normal", pending, "f", false);
    expect(
        file_second.action.has_value() && *file_second.action == EditorAction::GoToFileUnderCursor,
        "gf should map to go to file under cursor");

    KeyDispatch jump_back_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(jump_back_first.matched && jump_back_first.waiting_for_more, "g should wait for go");
    KeyDispatch jump_back_second = dispatch_key_sequence(keybindings, "normal", pending, "o", false);
    expect(
        jump_back_second.action.has_value() && *jump_back_second.action == EditorAction::JumpBack,
        "go should map to jump back");

    KeyDispatch jump_forward_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(jump_forward_first.matched && jump_forward_first.waiting_for_more, "g should wait for gi");
    KeyDispatch jump_forward_second = dispatch_key_sequence(keybindings, "normal", pending, "i", false);
    expect(
        jump_forward_second.action.has_value() && *jump_forward_second.action == EditorAction::JumpForward,
        "gi should map to jump forward");

    KeyDispatch diag_next_first = dispatch_key_sequence(keybindings, "normal", pending, "]", false);
    expect(diag_next_first.matched && diag_next_first.waiting_for_more, "] should wait for ]d");
    KeyDispatch diag_next_second = dispatch_key_sequence(keybindings, "normal", pending, "d", false);
    expect(
        diag_next_second.action.has_value() && *diag_next_second.action == EditorAction::NextDiagnostic,
        "]d should map to next diagnostic");

    KeyDispatch diag_prev_first = dispatch_key_sequence(keybindings, "normal", pending, "[", false);
    expect(diag_prev_first.matched && diag_prev_first.waiting_for_more, "[ should wait for [d");
    KeyDispatch diag_prev_second = dispatch_key_sequence(keybindings, "normal", pending, "d", false);
    expect(
        diag_prev_second.action.has_value() && *diag_prev_second.action == EditorAction::PreviousDiagnostic,
        "[d should map to previous diagnostic");

    KeyDispatch paste_after = dispatch_key_sequence(keybindings, "normal", pending, "p", false);
    expect(
        paste_after.action.has_value() && *paste_after.action == EditorAction::PasteAfter,
        "p should map to paste after");

    KeyDispatch delete_to_end = dispatch_key_sequence(keybindings, "normal", pending, "D", false);
    expect(delete_to_end.matched, "D should match");
    expect(delete_to_end.expansion.size() == 3, "D should expand to three tokens");
    expect(
        delete_to_end.expansion[0] == "v" && delete_to_end.expansion[1] == "$" && delete_to_end.expansion[2] == "d",
        "D should expand to visual line-end delete sequence");

    KeyDispatch inner_first = dispatch_key_sequence(keybindings, "visual", pending, "i", false);
    expect(inner_first.matched && inner_first.waiting_for_more, "visual i should wait for iw");
    KeyDispatch inner_second = dispatch_key_sequence(keybindings, "visual", pending, "w", false);
    expect(
        inner_second.action.has_value() && *inner_second.action == EditorAction::SelectInnerWord,
        "visual iw should map to select inner word");

    KeyDispatch change = dispatch_key_sequence(keybindings, "visual", pending, "c", false);
    expect(
        change.action.has_value() && *change.action == EditorAction::ChangeSelection,
        "visual c should map to change selection");

    KeyDispatch select_all = dispatch_key_sequence(keybindings, "normal", pending, "%", false);
    expect(
        select_all.action.has_value() && *select_all.action == EditorAction::SelectAll,
        "% should map to select all");

    KeyDispatch search_insert = dispatch_key_sequence(keybindings, "search", pending, "x", true);
    expect(
        search_insert.action.has_value() && *search_insert.action == EditorAction::SearchInsert,
        "search printable binding");
}

void test_config_file_selects_keybindings_and_colors() {
    char template_path[] = "/tmp/medit-config-XXXXXX";
    char *dir = mkdtemp(template_path);
    expect(dir != nullptr, "mkdtemp for config dir should succeed");

    std::string root = dir;
    std::string config_dir = root + "/.config";
    std::string medit_dir = config_dir + "/medit";
    std::filesystem::create_directories(medit_dir);

    {
        std::ofstream rc(config_dir + "/meditrc");
        rc << "keybindings = custom-keys.json\n";
        rc << "colors = amber.json\n";
        rc << "lsp = lsp.json\n";
        rc << "log = debug.log\n";
        rc << "syntax_config = syntax.json\n";
        rc << "syntax = cpp\n";
        rc << "right_justify_diagnostics = true\n";
        rc << "clipboard = shared-file\n";
        rc << "clipboard_file = clipboard.json\n";
        rc << "clipboard_osc52 = false\n";
    }
    {
        std::ofstream lsp(medit_dir + "/lsp.json");
        lsp << "{\n"
               "  \"servers\": [\n"
               "    {\n"
               "      \"name\": \"cpp\",\n"
               "      \"command\": \"clangd --background-index\",\n"
               "      \"language_id\": \"cpp\",\n"
               "      \"patterns\": [\"*.cpp\", \"*.hpp\", \"Makefile\"],\n"
               "      \"workspace\": {\n"
               "        \"markers\": [\"compile_commands.json\", \".git\"],\n"
               "        \"fallback\": \"file_directory\"\n"
               "      }\n"
               "    }\n"
               "  ]\n"
               "}\n";
    }
    {
        std::ofstream syntax(medit_dir + "/syntax.json");
        syntax << "{\n"
                  "  \"languages\": [\n"
                  "    {\n"
                  "      \"name\": \"python\",\n"
                  "      \"patterns\": [\"*.py\", \"*.pyi\", \".pythonrc\"],\n"
                  "      \"grammar_path\": \"grammars/libtree-sitter-python.so\",\n"
                  "      \"symbol_name\": \"tree_sitter_python\",\n"
                  "      \"highlights_path\": \"queries/python/highlights.scm\"\n"
                  "    }\n"
                  "  ]\n"
                  "}\n";
    }
    {
        std::ofstream keys(medit_dir + "/custom-keys.json");
        keys << "{\n"
                "  \"normal\": { \"z\": \"undo\", \"D\": [\"v\", \"$\", \"d\"] },\n"
                "  \"visual\": { \"esc\": \"enter_normal_mode\" },\n"
                "  \"insert\": { \"printable\": \"self_insert\" },\n"
                "  \"command\": { \"printable\": \"command_insert\" }\n"
                "}\n";
    }
    {
        std::ofstream colors(medit_dir + "/amber.json");
        colors << "{\n"
                  "  \"default_text\": { \"foreground\": \"default\", \"background\": \"default\", \"bold\": \"false\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"line_number\": { \"foreground\": \"yellow\", \"background\": \"default\", \"bold\": \"false\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"cursor_line\": { \"foreground\": \"default\", \"background\": \"black\", \"bold\": \"false\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"cursor_line_number\": { \"foreground\": \"yellow\", \"background\": \"black\", \"bold\": \"true\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"status_bar\": { \"foreground\": \"black\", \"background\": \"yellow\", \"bold\": \"true\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"message_bar\": { \"foreground\": \"yellow\", \"background\": \"default\", \"bold\": \"false\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"command_line\": { \"foreground\": \"white\", \"background\": \"yellow\", \"bold\": \"false\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"selection\": { \"foreground\": \"black\", \"background\": \"yellow\", \"bold\": \"false\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"search_match\": { \"foreground\": \"black\", \"background\": \"yellow\", \"bold\": \"true\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"search_match_current\": { \"foreground\": \"black\", \"background\": \"green\", \"bold\": \"true\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"syntax_keyword\": { \"foreground\": \"yellow\", \"background\": \"default\", \"bold\": \"true\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"syntax_string\": { \"foreground\": \"green\", \"background\": \"default\", \"bold\": \"false\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"syntax_comment\": { \"foreground\": \"blue\", \"background\": \"default\", \"bold\": \"false\", \"underline\": \"false\", \"reverse\": \"false\" },\n"
                  "  \"diagnostic_error\": { \"foreground\": \"red\", \"background\": \"default\", \"bold\": \"false\", \"underline\": \"true\", \"reverse\": \"false\" },\n"
                  "  \"diagnostic_warning\": { \"foreground\": \"yellow\", \"background\": \"default\", \"bold\": \"false\", \"underline\": \"true\", \"reverse\": \"false\" }\n"
                  "}\n";
    }

    EditorConfig config = load_editor_config_from_path(config_dir + "/meditrc");
    expect(config.keybindings_path.has_value(), "config should resolve keybindings path");
    expect(config.colors_path.has_value(), "config should resolve colors path");
    expect(config.lsp_path.has_value(), "config should resolve lsp path");
    expect(config.log_path.has_value(), "config should resolve log path");
    expect(config.syntax_config_path.has_value(), "config should resolve syntax config path");
    expect(config.clipboard.mode == ClipboardMode::SharedFile, "config should parse clipboard mode");
    expect(config.clipboard.shared_file_path.filename() == "clipboard.json", "config should parse clipboard file");
    expect(!config.clipboard.osc52, "config should parse clipboard osc52 flag");
    expect(
        config.keybindings_path->filename() == "custom-keys.json",
        "config should use configured keybindings file");
    expect(config.colors_path->filename() == "amber.json", "config should use configured colors file");
    expect(config.lsp_path->filename() == "lsp.json", "config should use configured lsp file");
    expect(config.log_path->filename() == "debug.log", "config should use configured log file");
    expect(config.syntax_config_path->filename() == "syntax.json", "config should use configured syntax config file");
    expect(config.lsp_servers.size() == 1, "config should parse lsp server rules");
    expect(config.syntax_languages.size() == 1, "config should parse syntax language rules");
    expect(config.syntax_languages[0].name == "python", "config should parse syntax language name");
    expect(config.syntax_languages[0].grammar_path.filename() == "libtree-sitter-python.so", "config should parse syntax grammar path");
    expect(config.syntax_languages[0].symbol_name == "tree_sitter_python", "config should parse syntax symbol");
    expect(config.syntax_languages[0].highlights_path.filename() == "highlights.scm", "config should parse syntax query path");
    expect(config.lsp_servers[0].name == "cpp", "config should parse lsp server name");
    expect(config.lsp_servers[0].command == "clangd --background-index", "config should parse lsp command");
    expect(config.lsp_servers[0].language_id == "cpp", "config should parse lsp language id");
    expect(config.lsp_servers[0].patterns.size() == 3, "config should parse lsp patterns");
    expect(config.syntax_languages[0].patterns.size() == 3, "config should parse syntax patterns");
    expect(config.lsp_servers[0].workspace.markers.size() == 2, "config should parse workspace markers");
    expect(
        config.lsp_servers[0].workspace.fallback == "file_directory",
        "config should parse workspace fallback");
    expect(config.syntax_name.has_value() && *config.syntax_name == "cpp", "config should parse syntax name");
    expect(config.right_justify_diagnostics, "config should parse right-justify diagnostics");

    KeyBindings keybindings = load_keybindings(config);
    std::vector<std::string> pending;
    KeyDispatch dispatch = dispatch_key_sequence(keybindings, "normal", pending, "z", false);
    expect(dispatch.action.has_value() && *dispatch.action == EditorAction::Undo, "custom keybinding should load");
    KeyDispatch alias = dispatch_key_sequence(keybindings, "normal", pending, "D", false);
    expect(alias.expansion.size() == 3, "custom keybinding alias should load");

    Theme theme = load_theme(config);
    TextStyle line_number = theme_style(theme, StyleRole::LineNumber);
    expect(line_number.foreground == COLOR_YELLOW, "custom color theme should load selected file");

    std::filesystem::remove_all(root);
}

void test_lsp_config_rejects_duplicate_patterns() {
    char template_path[] = "/tmp/medit-lsp-config-XXXXXX";
    char *dir = mkdtemp(template_path);
    expect(dir != nullptr, "mkdtemp for duplicate lsp config test should succeed");

    std::string root = dir;
    std::string config_dir = root + "/.config";
    std::string medit_dir = config_dir + "/medit";
    std::filesystem::create_directories(medit_dir);

    {
        std::ofstream rc(config_dir + "/meditrc");
        rc << "lsp = lsp.json\n";
    }
    {
        std::ofstream lsp(medit_dir + "/lsp.json");
        lsp << "{\n"
               "  \"servers\": [\n"
               "    {\"name\": \"cpp\", \"command\": \"clangd\", \"language_id\": \"cpp\", \"patterns\": [\"*.cpp\"]},\n"
               "    {\"name\": \"other\", \"command\": \"otherls\", \"language_id\": \"other\", \"patterns\": [\"*.cpp\"]}\n"
               "  ]\n"
               "}\n";
    }

    bool threw = false;
    try {
        (void)load_editor_config_from_path(config_dir + "/meditrc");
    } catch (const std::exception &) {
        threw = true;
    }
    expect(threw, "duplicate lsp pattern mappings should be rejected");
    std::filesystem::remove_all(root);
}

void test_infer_language_id() {
    EditorConfig config;
    config.lsp_servers.push_back({"python", "pyright-langserver --stdio", "python", {"*.py", "*.pyi", "*.pyw"}});
    config.lsp_servers.push_back({"json", "vscode-json-languageserver --stdio", "json", {"*.json", "*.jsonc"}});

    expect(infer_language_id(config, std::optional<std::string>("test.py")) == "python", "python pattern should infer python");
    expect(infer_language_id(config, std::optional<std::string>("settings.json")) == "json", "json pattern should infer json");
    expect(infer_language_id(config, std::optional<std::string>("main.cpp")) == "cpp", "cpp fallback should infer cpp");
    expect(infer_language_id(config, std::optional<std::string>("notes.txt")) == "text", "unknown filename should infer text");
    expect(infer_language_id(config, std::optional<std::string>("dir/.pythonrc")) == "text", "unmatched basename should not infer language");

    EditorConfig fallback;
    fallback.syntax_name = "cpp";
    expect(infer_language_id(fallback, std::nullopt) == "cpp", "syntax fallback should be used when no file path exists");
}

void test_infer_workspace_root() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit-workspace-root";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "project" / "src");
    std::filesystem::create_directories(root / "plain");

    {
        std::ofstream marker(root / "project" / "pyproject.toml");
        marker << "[project]\nname = \"demo\"\n";
    }

    LspServerConfig config;
    config.name = "python";
    config.command = "pyright-langserver --stdio";
    config.language_id = "python";
    config.patterns = {"*.py", "pyproject.toml"};
    config.workspace.markers = {"pyproject.toml", ".git"};
    config.workspace.fallback = "file_directory";

    std::filesystem::path workspace = infer_workspace_root(config, std::optional<std::string>((root / "project" / "src" / "main.py").string()));
    expect(workspace == root / "project", "workspace root should use nearest configured marker");

    std::filesystem::path fallback = infer_workspace_root(config, std::optional<std::string>((root / "plain" / "loose.py").string()));
    expect(fallback == root / "plain", "workspace root should fall back to file directory");

    std::filesystem::remove_all(root);
}

void test_process_utils_detect_missing_executables() {
    std::optional<std::string> first = first_command_word("clangd --background-index");
    expect(first.has_value() && *first == "clangd", "first command word should parse simple commands");

    std::optional<std::string> quoted = first_command_word("'custom tool' --flag");
    expect(quoted.has_value() && *quoted == "custom tool", "first command word should parse quoted commands");

    std::optional<std::string> missing = missing_executable_in_command("definitely-not-a-real-medit-command --version");
    expect(
        missing.has_value() && *missing == "definitely-not-a-real-medit-command",
        "missing command should be detected");

    std::optional<std::string> pipeline_missing =
        missing_executable_in_pipeline("rg --files | definitely-not-a-real-medit-command");
    expect(
        pipeline_missing.has_value() && *pipeline_missing == "definitely-not-a-real-medit-command",
        "missing pipeline executable should be detected");
}

void test_cpp_syntax_highlighting() {
    std::vector<std::u32string> lines = {
        utf8_to_u32("int main() {"),
        utf8_to_u32("  std::string value = \"hi\"; // note"),
        utf8_to_u32("  /* block"),
        utf8_to_u32("     comment */ return 0;"),
        utf8_to_u32("#include <vector>"),
    };

    EditorConfig config;
    SyntaxSelection selection{SyntaxEngine::LegacyCpp, "cpp"};
    auto highlights_result = highlight_document_syntax(lines, config, selection);
    expect(highlights_result.has_value(), "legacy syntax highlighting should succeed");
    std::vector<std::vector<HighlightSpan>> highlights = *highlights_result;
    expect(highlights.size() == lines.size(), "syntax highlighter should return one span list per line");
    expect(line_has_span(highlights[0], 0, 3, StyleRole::SyntaxKeyword), "cpp keyword should highlight");
    expect(line_has_span(highlights[1], 22, 26, StyleRole::SyntaxString), "string literal should highlight");
    expect(line_has_span(highlights[1], 28, 35, StyleRole::SyntaxComment), "line comment should highlight");
    expect(line_has_span(highlights[2], 2, 10, StyleRole::SyntaxComment), "block comment start should highlight");
    expect(line_has_span(highlights[3], 0, 15, StyleRole::SyntaxComment), "block comment continuation should highlight");
    expect(line_has_span(highlights[3], 16, 22, StyleRole::SyntaxKeyword), "keyword after block comment should highlight");
    expect(line_has_span(highlights[4], 0, 8, StyleRole::SyntaxKeyword), "preprocessor directive should highlight");

    SyntaxSelection detected_cpp = resolve_syntax_selection(config, std::optional<std::string>("sample.cpp"));
    expect(detected_cpp.engine == SyntaxEngine::LegacyCpp, "cpp extension should auto-detect legacy cpp syntax");

    SyntaxSelection detected_none = resolve_syntax_selection(config, std::optional<std::string>("notes.txt"));
    expect(detected_none.engine == SyntaxEngine::None, "non-code file should not auto-detect syntax");

    EditorConfig explicit_python;
    explicit_python.syntax_languages.push_back({"python", {"*.py", ".pythonrc"}, "python.so", "tree_sitter_python", "highlights.scm"});
    explicit_python.syntax_name = "python";
    SyntaxSelection configured = resolve_syntax_selection(explicit_python, std::optional<std::string>("notes.txt"));
    expect(configured.engine == SyntaxEngine::TreeSitter && configured.language_name == "python", "named tree-sitter syntax should resolve");

    auto missing_language = highlight_document_syntax(lines, config, {SyntaxEngine::TreeSitter, "missing"});
    expect(!missing_language.has_value(), "missing configured syntax should fail");
    expect(
        missing_language.error() == "configured syntax language not found: missing",
        "missing configured syntax should return explicit error");
}

void test_file_uri_normalization() {
    std::string uri = file_uri_for_path("tmp dir/file name.cpp");
    expect(uri.contains("%20"), "file uri should percent-encode spaces");

    std::string normalized = normalize_document_uri("file:///tmp%20dir/file%20name.cpp");
    expect(normalized == "file:///tmp%20dir/file%20name.cpp", "normalized file uri should preserve encoded absolute path");

    std::string roundtrip_path = file_path_from_uri(uri);
    expect(roundtrip_path.contains("tmp dir/file name.cpp"), "file uri should decode back to path");
}

void test_string_utilities() {
    expect(trim_ascii_whitespace("  value \t") == "value", "trim should remove surrounding ASCII whitespace");
    expect(ascii_lowercase("PyThOn") == "python", "ascii lowercase should normalize case");
    expect(normalize_extension("JSON") == ".json", "normalize extension should lowercase and add dot");
    expect(ellipsize_middle("abcdefghij", 7) == "ab...ij", "ellipsize should preserve both ends");
}

void test_lsp_message_framing() {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}";
    std::string encoded = encode_lsp_message(payload);
    std::string buffer = encoded;
    std::vector<std::string> messages = extract_lsp_messages(buffer);
    expect(messages.size() == 1, "lsp framing should extract one message");
    expect(messages[0] == payload, "lsp framing should preserve payload");
    expect(buffer.empty(), "lsp framing should consume buffer");
}

void test_lsp_service_roundtrip() {
#if defined(__unix__) || defined(__APPLE__)
    char dir_template[] = "/tmp/medit-lsp-XXXXXX";
    char *dir = mkdtemp(dir_template);
    expect(dir != nullptr, "mkdtemp for lsp test should succeed");
    std::string path = std::string(dir) + "/sample.txt";
    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    expect(fd >= 0, "mkstemp for lsp test should succeed");
    close(fd);
    {
        std::ofstream file(path);
        file << "abc";
    }

    LspServerConfig config;
    config.name = "text";
    config.command = std::string("python3 ") + std::filesystem::current_path().string() + "/tests/fake_lsp_server.py";
    config.language_id = "text";
    config.patterns = {"*.txt"};

    EditorRuntime runtime;
    runtime.add_service(std::make_unique<LspService>(config));
    runtime.start_services();

    EditorCore core;
    expect(core.load_file(path), "load file for lsp roundtrip test");

    auto drive_runtime = [&](int iterations) {
        for (int i = 0; i < iterations; ++i) {
            runtime.process(core);
            for (const ServiceEvent &event : runtime.take_service_events()) {
                if (event.command) {
                    apply_editor_command(core, *event.command);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };

    drive_runtime(30);
    expect(core.diagnostics().size() == 1, "lsp roundtrip should apply open diagnostics");
    expect(u32_to_utf8(core.diagnostics()[0].message) == "open diagnostic", "lsp open diagnostic message");

    core.insert_codepoint(U'x');
    drive_runtime(30);
    expect(core.diagnostics().size() == 1, "lsp roundtrip should update diagnostics after change");
    expect(u32_to_utf8(core.diagnostics()[0].message) == "changed diagnostic", "lsp changed diagnostic message");

    ServiceRequest request;
    request.type = ServiceRequestType::GoToDefinition;
    request.document_uri = core.document_uri();
    request.utf16_position = core.utf16_position_for_position({0, 0});
    runtime.dispatch_service_request(request);

    bool saw_definition = false;
    for (int i = 0; i < 30 && !saw_definition; ++i) {
        runtime.poll_services();
        for (const ServiceEvent &event : runtime.take_service_events()) {
            if (!event.command || event.command->type != EditorCommandType::OpenLocation) {
                continue;
            }
            expect(event.command->document_uri == core.document_uri(), "definition should target same document in fake server");
            expect(event.command->position.has_value(), "definition should include a target position");
            expect(event.command->position->row == 0 && event.command->position->column == 1, "definition should decode target position");
            saw_definition = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(saw_definition, "lsp roundtrip should produce a definition location");

    runtime.stop_services();
    std::filesystem::remove_all(dir);
#endif
}

void test_lsp_service_reports_startup_failures() {
#if defined(__unix__) || defined(__APPLE__)
    LspServerConfig config;
    config.name = "broken";
    config.command = "echo mac-startup-failure >&2; exit 1";
    config.language_id = "text";
    config.patterns = {"*"};

    LspService service(config);
    service.start();

    bool saw_stderr = false;
    bool saw_exit = false;
    for (int i = 0; i < 30; ++i) {
        for (const ServiceEvent &event : service.poll()) {
            if (!event.command || event.command->type != EditorCommandType::SetStatusMessage) {
                continue;
            }
            if (event.command->message.contains("mac-startup-failure")) {
                saw_stderr = true;
            }
            if (event.command->message.contains("LSP exited before initialize")) {
                saw_exit = true;
            }
        }
        if (saw_stderr && saw_exit) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    service.stop();
    expect(saw_stderr, "lsp service should surface server stderr");
    expect(saw_exit, "lsp service should report exit before initialize");
#endif
}

class RecordingService : public EditorService {
  public:
    explicit RecordingService(std::optional<int> interval_ms = std::nullopt) : interval_ms_(interval_ms) {}

    std::string name() const override {
        return "recording";
    }

    void start() override {
        started = true;
    }

    void stop() override {
        stopped = true;
    }

    void handle_editor_event(const EditorEvent &event) override {
        received_events.push_back(event);
        queued_events.push_back(
            {ServiceEventType::Notification,
             name(),
             event.type == EditorEventType::DocumentChanged ? "document_changed" : "editor_event",
             EditorCommand{EditorCommandType::SetStatusMessage, std::nullopt, {}, {}, std::nullopt, "recorded"},
             event.document_uri,
             event.document_version,
             event.range,
             event.text});
    }

    std::vector<ServiceEvent> poll() override {
        std::vector<ServiceEvent> events = std::move(queued_events);
        queued_events.clear();
        return events;
    }

    std::optional<int> poll_interval_ms() const override {
        return interval_ms_;
    }

    bool started = false;
    bool stopped = false;
    std::vector<EditorEvent> received_events;
    std::vector<ServiceEvent> queued_events;

  private:
    std::optional<int> interval_ms_;
};

void test_editor_runtime_service_boundary() {
    EditorRuntime runtime;
    auto service = std::make_unique<RecordingService>();
    RecordingService *service_ptr = service.get();

    runtime.add_service(std::move(service));
    expect(runtime.service_count() == 1, "runtime should track registered service");
    expect(!runtime.started(), "runtime should not start automatically");

    runtime.start_services();
    expect(runtime.started(), "runtime should enter started state");
    expect(service_ptr->started, "runtime should start registered services");

    std::vector<ServiceEvent> lifecycle_events = runtime.take_service_events();
    expect(lifecycle_events.size() == 1, "starting runtime should emit one service lifecycle event");
    expect(lifecycle_events[0].type == ServiceEventType::ServiceStarted, "runtime should emit service started");

    EditorCore core;
    core.insert_codepoint(U'a');
    runtime.dispatch_editor_events(core);
    expect(core.pending_events().empty(), "runtime dispatch should drain core editor events");
    expect(service_ptr->received_events.size() == 2, "service should receive change and cursor events");

    runtime.poll_services();
    std::vector<ServiceEvent> service_events = runtime.take_service_events();
    expect(service_events.size() == 2, "polling runtime should collect service notifications");
    expect(service_events[0].service_name == "recording", "service event should carry service name");
    expect(service_events[0].command.has_value(), "service event should carry editor command");
    expect(service_events[0].command->type == EditorCommandType::SetStatusMessage, "service command should preserve type");
    expect(service_events[0].document_uri.has_value(), "service event should carry document identity");
    expect(service_events[0].document_version == core.document_version(), "service event should carry document version");

    runtime.stop_services();
    expect(!runtime.started(), "runtime stop should clear started state");
    expect(service_ptr->stopped, "runtime should stop registered services");
    std::vector<ServiceEvent> stopped_events = runtime.take_service_events();
    expect(stopped_events.size() == 1, "stopping runtime should emit one lifecycle event");
    expect(stopped_events[0].type == ServiceEventType::ServiceStopped, "runtime should emit service stopped");
}

void test_editor_runtime_idle_timeout() {
    EditorRuntime runtime;
    runtime.add_service(std::make_unique<RecordingService>(150));
    runtime.add_service(std::make_unique<RecordingService>(25));
    runtime.add_service(std::make_unique<RecordingService>());

    expect(!runtime.idle_wait_timeout_ms().has_value(), "stopped runtime should not request idle wakeups");

    runtime.start_services();
    std::optional<int> timeout_ms = runtime.idle_wait_timeout_ms();
    expect(timeout_ms.has_value(), "started runtime with polling services should request idle wakeups");
    expect(*timeout_ms == 25, "runtime should choose the shortest requested poll interval");

    runtime.stop_services();
    expect(!runtime.idle_wait_timeout_ms().has_value(), "stopped runtime should clear idle wakeups");
}

void test_editor_session_buffers_and_clipboard() {
    EditorSession session;
    ClipboardConfig clipboard_config;
    clipboard_config.mode = ClipboardMode::Internal;
    session.configure_clipboard(clipboard_config);
    expect(session.buffer_count() == 1, "session should start with one buffer");
    expect(session.active_buffer().core.display_file_name() == "[No Name]", "initial buffer should be unnamed");

    session.active_buffer().core.insert_text({0, 0}, utf8_to_u32("alpha"));
    session.active_buffer().core.begin_selection();
    session.active_buffer().core.move_right();
    expect(session.active_buffer().core.yank_selection(), "initial buffer yank should succeed");
    session.capture_active_clipboard();

    std::size_t first_id = session.active_buffer_id();
    session.new_buffer(true);
    expect(session.buffer_count() == 2, "new buffer should be added");
    expect(session.active_buffer_id() != first_id, "new buffer should become active");
    expect(session.active_buffer().core.paste_after_cursor(), "shared clipboard should paste into another buffer");
    expect_text(session.active_buffer().core, "al", "paste into second buffer should use shared clipboard");

    session.active_buffer().core.insert_text({0, 2}, utf8_to_u32("beta"));
    expect_text(session.active_buffer().core, "albeta", "second buffer should keep independent text");
    expect(session.switch_to_id(first_id), "switch back to first buffer");
    expect_text(session.active_buffer().core, "alpha", "first buffer text should be preserved");
}

void test_editor_session_shared_file_clipboard() {
    char template_path[] = "/tmp/medit-clipboard-XXXXXX";
    char *dir = mkdtemp(template_path);
    expect(dir != nullptr, "mkdtemp for clipboard dir should succeed");

    std::filesystem::path clipboard_path = std::filesystem::path(dir) / "clipboard.json";
    ClipboardConfig clipboard_config;
    clipboard_config.mode = ClipboardMode::SharedFile;
    clipboard_config.shared_file_path = clipboard_path;
    clipboard_config.osc52 = false;

    EditorSession first;
    first.configure_clipboard(clipboard_config);
    first.active_buffer().core.insert_text({0, 0}, utf8_to_u32("alpha"));
    first.active_buffer().core.begin_selection();
    first.active_buffer().core.move_right();
    expect(first.active_buffer().core.yank_selection(), "first session yank should succeed");
    first.capture_active_clipboard();

    EditorSession second;
    second.configure_clipboard(clipboard_config);
    second.sync_active_clipboard();
    expect(second.active_buffer().core.paste_after_cursor(), "second session should paste shared clipboard");
    expect_text(second.active_buffer().core, "al", "second session should see shared clipboard contents");

    std::filesystem::remove_all(dir);
}

void test_editor_session_open_and_close_rules() {
    EditorSession session;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "medit_session_test";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path first_path = temp_dir / "first.txt";
    std::filesystem::path second_path = temp_dir / "second.txt";

    {
        std::ofstream output(first_path);
        output << "first\n";
    }
    {
        std::ofstream output(second_path);
        output << "second\n";
    }

    EditorBuffer *first = session.open_file(first_path.string(), true);
    expect(first != nullptr, "open first file should succeed");
    expect(session.buffer_count() == 2, "opening a file should keep current buffer and add one");
    expect(first->core.display_file_name() == first_path.string(), "opened file name should match path");

    EditorBuffer *second = session.open_file(second_path.string(), true);
    expect(second != nullptr, "open second file should succeed");
    expect(session.buffer_count() == 3, "opening second file should add another buffer");

    session.active_buffer().core.insert_codepoint(U'!');
    expect(!session.close_active_buffer(false), "closing dirty buffer without force should fail");
    expect(session.buffer_count() == 3, "failed close should keep buffer count");
    std::vector<EditorEvent> close_events;
    expect(session.close_active_buffer(true, &close_events), "force closing dirty buffer should succeed");
    expect(session.buffer_count() == 2, "force close should remove buffer");
    expect(!close_events.empty(), "closing a buffer should emit close events");
    expect_event_type(close_events.back(), EditorEventType::DocumentClosed, "close should emit document closed");
}

void test_editor_session_open_missing_file_creates_named_buffer() {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "medit_session_missing_file";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path path = temp_dir / "missing.txt";

    EditorSession session;
    EditorBuffer *buffer = session.open_file(path.string(), true);
    expect(buffer != nullptr, "opening missing file should create a buffer");
    expect(buffer->core.file_path().has_value() && *buffer->core.file_path() == path.string(), "missing-file buffer should keep requested path");
    expect_text(buffer->core, "", "missing-file buffer should start empty");

    buffer->core.insert_codepoint(U'a');
    expect(buffer->core.save_current_file(), "saving missing-file buffer should create file");

    EditorCore reopened;
    expect(reopened.load_file(path.string()), "created file should load");
    expect_text(reopened, "a", "created file should contain saved text");

    std::filesystem::remove_all(temp_dir);
}

void test_editor_session_open_multiple_files() {
    EditorSession session;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "medit_multi_open_test";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path first_path = temp_dir / "first.txt";
    std::filesystem::path second_path = temp_dir / "second.txt";

    {
        std::ofstream output(first_path);
        output << "first\n";
    }
    {
        std::ofstream output(second_path);
        output << "second\n";
    }

    EditorCore &first_core = session.active_buffer().core;
    expect(first_core.load_file(first_path.string()), "initial startup file should load into first buffer");
    std::size_t first_id = session.active_buffer_id();
    EditorBuffer *second = session.open_file(second_path.string(), false);
    expect(second != nullptr, "second startup file should open in another buffer");
    expect(session.buffer_count() == 2, "opening two startup files should create two buffers");
    expect(session.active_buffer_id() == first_id, "first startup file should remain active");
    expect_text(session.active_buffer().core, "first", "first startup buffer should preserve first file");
    expect_text(second->core, "second", "second startup buffer should contain second file");

    std::filesystem::remove_all(temp_dir);
}

}  // namespace

int main() {
    try {
        test_insert_unicode_and_undo();
        test_newline_backspace_and_join();
        test_insert_mode_soft_tab_and_shift_tab();
        test_visual_range_semantics_and_delete();
        test_linewise_selection_range_and_delete();
        test_linewise_delete_and_paste();
        test_yank_and_paste();
        test_indent_and_outdent_lines();
        test_replace_selection_with_yank();
        test_file_io_and_dirty_tracking();
        test_open_empty_missing_file_and_save();
        test_navigation();
        test_find_and_till_character_motions();
        test_word_object_ranges();
        test_extend_selection_to_word_object();
        test_generic_range_edit_api();
        test_text_edit_transactions();
        test_text_edit_transaction_rejects_overlaps();
        test_document_version_is_monotonic();
        test_document_identity_and_events();
        test_open_save_and_save_as_events();
        test_unicode_position_conversions();
        test_diagnostics_storage_and_events();
        test_annotations_storage_and_events();
        test_diagnostics_follow_document_switches();
        test_editor_command_entry_points();
        test_compound_edit_undo();
        test_keybinding_dispatch();
        test_config_file_selects_keybindings_and_colors();
        test_lsp_config_rejects_duplicate_patterns();
        test_infer_language_id();
        test_infer_workspace_root();
        test_process_utils_detect_missing_executables();
        test_cpp_syntax_highlighting();
        test_file_uri_normalization();
        test_string_utilities();
        test_lsp_message_framing();
        test_lsp_service_roundtrip();
        test_lsp_service_reports_startup_failures();
        test_editor_runtime_service_boundary();
        test_editor_runtime_idle_timeout();
        test_editor_session_buffers_and_clipboard();
        test_editor_session_shared_file_clipboard();
        test_editor_session_open_and_close_rules();
        test_editor_session_open_missing_file_creates_named_buffer();
        test_editor_session_open_multiple_files();
    } catch (const std::exception &error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
