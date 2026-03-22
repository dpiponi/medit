#include "config.hpp"
#include "editor_commands.hpp"
#include "editor_core.hpp"
#include "editor_ex_command_completion.hpp"
#include "editor_internal.hpp"
#include "editor_session.hpp"
#include "editor_windows.hpp"
#include "lsp_service.hpp"
#include "process_utils.hpp"
#include "services.hpp"
#include "syntax.hpp"
#include "string_utils.hpp"
#include "theme.hpp"

#include <cstdio>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <curses.h>
#include <fcntl.h>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

import theme;
import keybindings;

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

void expect_editor_state_sane(const EditorState &state, const std::string &context);

void expect_event_type(const EditorEvent &event, EditorEventType expected, const std::string &message) {
    expect(event.type == expected, message);
}

const KeyHint *find_key_hint(const std::vector<KeyHint> &hints, const std::string &token) {
    for (const KeyHint &hint : hints) {
        if (hint.token == token) {
            return &hint;
        }
    }
    return nullptr;
}

struct ScopedTestScreen {
    FILE *input = nullptr;
    FILE *output = nullptr;
    SCREEN *screen = nullptr;

    ScopedTestScreen() {
        setenv("TERM", "xterm-256color", 1);
        input = fopen("/dev/null", "r");
        output = fopen("/dev/null", "w");
        if (input == nullptr || output == nullptr) {
            throw std::runtime_error("could not open /dev/null for curses test screen");
        }
        screen = newterm("xterm-256color", output, input);
        if (screen == nullptr) {
            throw std::runtime_error("could not initialize curses test screen");
        }
        set_term(screen);
        raw();
        noecho();
        keypad(stdscr, TRUE);
        timeout(-1);
    }

    ~ScopedTestScreen() {
        if (screen != nullptr) {
            endwin();
            delscreen(screen);
        }
        if (input != nullptr) {
            fclose(input);
        }
        if (output != nullptr) {
            fclose(output);
        }
    }
};

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
    expect(core.insert_soft_tab(8, 4, false), "soft tab at line start should succeed");
    expect_text(core, "    ", "soft tab inserts spaces to next shiftwidth stop");
    expect(core.insert_soft_tab(8, 4, false), "second soft tab should succeed");
    expect_text(core, "        ", "soft tab advances to the next shiftwidth stop");
    expect(core.outdent_before_cursor(8, 4), "shift-tab style outdent should succeed");
    expect_text(core, "    ", "shift-tab outdents to previous shiftwidth stop");
    expect(core.insert_text({0, 4}, utf8_to_u32("x")), "insert non-whitespace text");
    core.set_cursor({0, 5});
    expect(core.insert_soft_tab(8, 4, false), "soft tab outside indentation should insert a tab");
    expect_text(core, "    x\t", "soft tab outside leading whitespace inserts a tab");

    EditorCore spaces;
    expect(spaces.insert_soft_tab(8, 2, true), "expandtab should insert spaces");
    expect_text(spaces, "  ", "expandtab inserts spaces outside indentation too");
}

void test_autoindent_newline() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("    alpha")), "seed autoindent buffer");
    core.set_cursor({0, 9});
    core.insert_newline_with_autoindent();
    expect_text(core, "    alpha\n    ", "autoindent copies leading indentation to the new line");
    expect(core.undo(), "undo autoindent newline");
    expect_text(core, "    alpha", "undo autoindent newline restores the original line");
}

void test_autoindent_open_line_below() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("    alpha")), "seed open-line-below autoindent buffer");
    core.set_cursor({0, 2});
    core.open_line_below_with_autoindent();
    expect_text(core, "    alpha\n    ", "open line below copies leading indentation");
    expect(core.undo(), "undo autoindented open line below");
    expect_text(core, "    alpha", "undo autoindented open line below restores the original line");
}

void test_autoindent_open_line_above() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("    alpha")), "seed open-line-above autoindent buffer");
    core.set_cursor({0, 2});
    core.open_line_above_with_autoindent();
    expect_text(core, "    \n    alpha", "open line above copies leading indentation");
    expect_cursor(core, {0, 4}, "open line above with autoindent moves cursor after indentation");
    expect(core.undo(), "undo autoindented open line above");
    expect_text(core, "    alpha", "undo autoindented open line above restores the original line");
}

void test_open_line_below_at_eof_then_newline() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("one\ntwo")), "seed eof open-line-below buffer");
    core.move_to_last_line();
    core.move_line_start();
    core.open_line_below();
    expect_text(core, "one\ntwo\n", "open line below at eof inserts trailing blank line");
    expect_cursor(core, {2, 0}, "open line below at eof moves cursor to inserted line");

    core.insert_newline();
    expect_text(core, "one\ntwo\n\n", "newline on inserted eof line adds another blank line");
    expect_cursor(core, {3, 0}, "newline after eof open line stays at new bottom line");
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

void test_matching_pair_navigation() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("alpha(foo[bar{baz}])\nomega")), "seed matching pair buffer");

    core.set_cursor({0, 5});
    std::optional<Position> pair = matching_pair_cursor(core);
    expect(pair.has_value() && *pair == Position{0, 5}, "cursor on opening paren should identify pair anchor");
    std::optional<Position> match = matching_pair_position(core, *pair);
    expect(match.has_value() && *match == Position{0, 19}, "opening paren should match closing paren");

    core.set_cursor({0, 19});
    pair = matching_pair_cursor(core);
    expect(pair.has_value() && *pair == Position{0, 19}, "cursor on closing paren should identify pair anchor");
    match = matching_pair_position(core, *pair);
    expect(match.has_value() && *match == Position{0, 5}, "closing paren should match opening paren");

    core.set_cursor({0, 13});
    pair = matching_pair_cursor(core);
    expect(pair.has_value() && *pair == Position{0, 13}, "cursor on opening brace should identify nested anchor");
    match = matching_pair_position(core, *pair);
    expect(match.has_value() && *match == Position{0, 17}, "opening brace should match closing brace");
}

void test_indent_and_outdent_lines() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("one\ntwo\n\tthree")), "seed indent buffer");
    expect(core.indent_lines(0, 1, 2, true, 8), "indent first two lines");
    expect_text(core, "  one\n  two\n\tthree", "indent adds spaces to selected lines");
    expect(core.undo(), "undo indent");
    expect_text(core, "one\ntwo\n\tthree", "undo indent restores text");

    expect(core.outdent_lines(2, 2, 2, 8), "outdent tab-indented line");
    expect_text(core, "one\ntwo\nthree", "outdent removes a leading tab");
    expect(core.undo(), "undo outdent");
    expect_text(core, "one\ntwo\n\tthree", "undo outdent restores text");

    expect(core.indent_lines(0, 0, 8, false, 8), "noexpandtab indent should use tabs");
    expect_text(core, "\tone\ntwo\n\tthree", "noexpandtab indent uses a tab when possible");
}

void test_substitute_regex() {
    EditorCore core;
    expect(core.insert_text({0, 0}, utf8_to_u32("alpha beta alpha\nbeta alpha")), "seed substitute buffer");

    std::string error_message;
    std::size_t current_line = core.substitute_regex(0, 0, "alpha", "omega", false, error_message);
    expect(error_message.empty(), "single substitute should not report an error");
    expect(current_line == 1, "single substitute should replace one match");
    expect_text(core, "omega beta alpha\nbeta alpha", "single substitute should affect only first match on the line");
    expect(core.undo(), "undo single substitute should succeed");
    expect_text(core, "alpha beta alpha\nbeta alpha", "undo single substitute should restore text");

    std::size_t whole_buffer = core.substitute_regex(0, 1, "alpha", "omega", true, error_message);
    expect(error_message.empty(), "global substitute should not report an error");
    expect(whole_buffer == 3, "global substitute should count all matches");
    expect_text(core, "omega beta omega\nbeta omega", "global substitute should replace across the range");
    expect(core.undo(), "undo global substitute should succeed");
    expect_text(core, "alpha beta alpha\nbeta alpha", "undo global substitute should restore text");

    std::size_t invalid = core.substitute_regex(0, 1, "(", "omega", true, error_message);
    expect(invalid == 0, "invalid regex should not apply substitutions");
    expect(error_message == "invalid regex", "invalid regex should return explicit error");

    error_message.clear();
    std::size_t whole_match = core.substitute_regex(0, 1, "(alpha)", "[\\1]&", true, error_message);
    expect(error_message.empty(), "capture substitute should not report an error");
    expect(whole_match == 3, "capture substitute should count all matches");
    expect_text(core, "[alpha]alpha beta [alpha]alpha\nbeta [alpha]alpha", "capture substitute should support vi-style replacements");
    expect(core.undo(), "undo capture substitute should succeed");
    expect_text(core, "alpha beta alpha\nbeta alpha", "undo capture substitute should restore text");

    error_message.clear();
    std::size_t selected = core.substitute_regex_in_range({{0, 6}, {0, 10}}, "beta", "B&", false, error_message);
    expect(error_message.empty(), "range substitute should not report an error");
    expect(selected == 1, "range substitute should count one match");
    expect_text(core, "alpha Bbeta alpha\nbeta alpha", "range substitute should only affect selected text");
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
    InlineAnnotation note{
        {{0, 0}, {0, 1}},
        AnnotationSeverity::Info,
        AnnotationKind::Note,
        "note",
        utf8_to_u32("hello\nworld"),
        std::nullopt};
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

void test_lua_annotations_storage_and_events() {
    EditorCore core;
    InlineAnnotation note{
        {{0, 0}, {0, 1}},
        AnnotationSeverity::Info,
        AnnotationKind::Note,
        "note",
        utf8_to_u32("editor"),
        std::nullopt};
    InlineAnnotation lua_note{
        {{0, 0}, {0, 1}},
        AnnotationSeverity::Warning,
        AnnotationKind::Note,
        "lua",
        utf8_to_u32("lua"),
        TextStyle{COLOR_BLUE, -1, true, false, false}};
    core.set_annotations({note});
    core.take_events();

    core.set_lua_annotations({lua_note});
    expect(core.annotations().size() == 1, "editor annotations should be preserved");
    expect(core.lua_annotations().size() == 1, "lua annotations should be stored separately");
    expect(core.projected_annotations().size() == 2, "projected annotations should include editor and lua annotations");
    expect(core.projected_annotations()[1].style_override.has_value(), "lua annotations should preserve style override");

    std::vector<EditorEvent> events = core.take_events();
    expect(events.size() == 1, "setting lua annotations should emit one event");
    expect_event_type(events[0], EditorEventType::AnnotationsChanged, "setting lua annotations emits annotations changed");

    core.clear_lua_annotations();
    expect(core.annotations().size() == 1, "clearing lua annotations should not clear editor annotations");
    expect(core.lua_annotations().empty(), "clearing lua annotations should remove lua annotations");
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
        {{{0, 0}, {0, 1}}, AnnotationSeverity::Info, AnnotationKind::Note, "cmd", utf8_to_u32("note"), std::nullopt}
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

    KeyDispatch hover_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(hover_first.matched && hover_first.waiting_for_more, "g should wait for gk");
    KeyDispatch hover_second = dispatch_key_sequence(keybindings, "normal", pending, "k", false);
    expect(hover_second.action.has_value() && *hover_second.action == EditorAction::ShowHover, "gk should map to hover");

    KeyDispatch printable = dispatch_key_sequence(keybindings, "insert", pending, "x", true);
    expect(printable.action.has_value() && *printable.action == EditorAction::SelfInsert, "insert printable binding");

    KeyDispatch insert_tab = dispatch_key_sequence(keybindings, "insert", pending, "tab", false);
    expect(insert_tab.action.has_value() && *insert_tab.action == EditorAction::InsertSoftTab, "insert tab binding");

    KeyDispatch insert_shift_tab = dispatch_key_sequence(keybindings, "insert", pending, "shift-tab", false);
    expect(
        insert_shift_tab.action.has_value() && *insert_shift_tab.action == EditorAction::InsertOutdent,
        "insert shift-tab binding");

    KeyDispatch insert_completion = dispatch_key_sequence(keybindings, "insert", pending, "ctrl-p", false);
    expect(
        insert_completion.action.has_value() && *insert_completion.action == EditorAction::ShowCompletion,
        "insert ctrl-p should map to completion");

    KeyDispatch show_hints = dispatch_key_sequence(keybindings, "normal", pending, "?", false);
    expect(
        show_hints.action.has_value() && *show_hints.action == EditorAction::ShowKeyHints,
        "? should map to key hints");

    KeyDispatch special = dispatch_key_sequence(keybindings, "normal", pending, "pagedown", false);
    expect(special.action.has_value() && *special.action == EditorAction::PageDown, "pagedown binding");

    KeyDispatch shift_left = dispatch_key_sequence(keybindings, "normal", pending, "shift-left", false);
    expect(
        shift_left.action.has_value() && *shift_left.action == EditorAction::VisualMoveLeft,
        "shift-left should map to visual left");

    KeyDispatch shift_right = dispatch_key_sequence(keybindings, "normal", pending, "shift-right", false);
    expect(
        shift_right.action.has_value() && *shift_right.action == EditorAction::VisualMoveRight,
        "shift-right should map to visual right");

    KeyDispatch shift_up = dispatch_key_sequence(keybindings, "normal", pending, "shift-up", false);
    expect(
        shift_up.action.has_value() && *shift_up.action == EditorAction::VisualMoveScreenUp,
        "shift-up should map to visual screen up");

    KeyDispatch shift_down = dispatch_key_sequence(keybindings, "normal", pending, "shift-down", false);
    expect(
        shift_down.action.has_value() && *shift_down.action == EditorAction::VisualMoveScreenDown,
        "shift-down should map to visual screen down");

    KeyDispatch home = dispatch_key_sequence(keybindings, "normal", pending, "home", false);
    expect(home.action.has_value() && *home.action == EditorAction::MoveLineStart, "home should map to line start");

    KeyDispatch shift_home = dispatch_key_sequence(keybindings, "normal", pending, "shift-home", false);
    expect(
        shift_home.action.has_value() && *shift_home.action == EditorAction::VisualMoveLineStart,
        "shift-home should map to visual line start");

    KeyDispatch end = dispatch_key_sequence(keybindings, "normal", pending, "end", false);
    expect(end.action.has_value() && *end.action == EditorAction::MoveLineEnd, "end should map to line end");

    KeyDispatch shift_end = dispatch_key_sequence(keybindings, "normal", pending, "shift-end", false);
    expect(
        shift_end.action.has_value() && *shift_end.action == EditorAction::VisualMoveLineEnd,
        "shift-end should map to visual line end");

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

    KeyDispatch split_first = dispatch_key_sequence(keybindings, "normal", pending, "ctrl-w", false);
    expect(split_first.matched && split_first.waiting_for_more, "ctrl-w should wait for window command");
    KeyDispatch split_second = dispatch_key_sequence(keybindings, "normal", pending, "s", false);
    expect(
        split_second.action.has_value() && *split_second.action == EditorAction::SplitHorizontal,
        "ctrl-w s should split horizontally");

    KeyDispatch focus_first = dispatch_key_sequence(keybindings, "normal", pending, "ctrl-w", false);
    expect(focus_first.matched && focus_first.waiting_for_more, "ctrl-w should wait for focus command");
    KeyDispatch focus_second = dispatch_key_sequence(keybindings, "normal", pending, "l", false);
    expect(
        focus_second.action.has_value() && *focus_second.action == EditorAction::FocusWindowRight,
        "ctrl-w l should focus right window");

    KeyDispatch focus_arrow_first = dispatch_key_sequence(keybindings, "normal", pending, "ctrl-w", false);
    expect(focus_arrow_first.matched && focus_arrow_first.waiting_for_more, "ctrl-w should wait for arrow focus command");
    KeyDispatch focus_arrow_second = dispatch_key_sequence(keybindings, "normal", pending, "left", false);
    expect(
        focus_arrow_second.action.has_value() && *focus_arrow_second.action == EditorAction::FocusWindowLeft,
        "ctrl-w left should focus left window");

    KeyDispatch suspend = dispatch_key_sequence(keybindings, "normal", pending, "ctrl-z", false);
    expect(suspend.action.has_value() && *suspend.action == EditorAction::Suspend, "ctrl-z binding");

    KeyDispatch linewise = dispatch_key_sequence(keybindings, "normal", pending, "V", false);
    expect(
        linewise.action.has_value() && *linewise.action == EditorAction::EnterVisualLineMode,
        "V should map to linewise visual mode");

    KeyDispatch visual_left = dispatch_key_sequence(keybindings, "normal", pending, "H", false);
    expect(
        visual_left.action.has_value() && *visual_left.action == EditorAction::VisualMoveLeft,
        "H should map to visual left");

    KeyDispatch visual_down = dispatch_key_sequence(keybindings, "normal", pending, "J", false);
    expect(
        visual_down.action.has_value() && *visual_down.action == EditorAction::VisualMoveDown,
        "J should map to visual down");

    KeyDispatch visual_up = dispatch_key_sequence(keybindings, "normal", pending, "K", false);
    expect(
        visual_up.action.has_value() && *visual_up.action == EditorAction::VisualMoveUp,
        "K should map to visual up");

    KeyDispatch visual_right = dispatch_key_sequence(keybindings, "normal", pending, "L", false);
    expect(
        visual_right.action.has_value() && *visual_right.action == EditorAction::VisualMoveRight,
        "L should map to visual right");

    KeyDispatch filter = dispatch_key_sequence(keybindings, "visual", pending, "|", false);
    expect(
        filter.action.has_value() && *filter.action == EditorAction::FilterSelection,
        "| should map to filter selection in visual mode");

    KeyDispatch sed = dispatch_key_sequence(keybindings, "visual", pending, "S", false);
    expect(
        sed.action.has_value() && *sed.action == EditorAction::SedSelection,
        "S should map to sed selection in visual mode");

    KeyDispatch visual_command = dispatch_key_sequence(keybindings, "visual", pending, ":", false);
    expect(
        visual_command.action.has_value() && *visual_command.action == EditorAction::EnterCommandMode,
        ": should enter command mode in visual mode");

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

    KeyDispatch command_completion = dispatch_key_sequence(keybindings, "command", pending, "tab", false);
    expect(
        command_completion.action.has_value() &&
            *command_completion.action == EditorAction::ShowCommandCompletion,
        "command tab should show command completion");

    KeyDispatch command_left = dispatch_key_sequence(keybindings, "command", pending, "left", false);
    expect(command_left.action.has_value() && *command_left.action == EditorAction::PromptMoveLeft, "command left should move prompt cursor");

    KeyDispatch command_right = dispatch_key_sequence(keybindings, "command", pending, "right", false);
    expect(command_right.action.has_value() && *command_right.action == EditorAction::PromptMoveRight, "command right should move prompt cursor");

    KeyDispatch command_home = dispatch_key_sequence(keybindings, "command", pending, "home", false);
    expect(command_home.action.has_value() && *command_home.action == EditorAction::PromptMoveStart, "command home should move prompt cursor to start");

    KeyDispatch command_end = dispatch_key_sequence(keybindings, "command", pending, "end", false);
    expect(command_end.action.has_value() && *command_end.action == EditorAction::PromptMoveEnd, "command end should move prompt cursor to end");

    KeyDispatch find = dispatch_key_sequence(keybindings, "normal", pending, "f", false);
    expect(find.action.has_value() && *find.action == EditorAction::FindForward, "f should map to find forward");

    KeyDispatch visual_find = dispatch_key_sequence(keybindings, "normal", pending, "F", false);
    expect(
        visual_find.action.has_value() && *visual_find.action == EditorAction::VisualFindForward,
        "F should map to visual find forward");

    KeyDispatch till = dispatch_key_sequence(keybindings, "normal", pending, "t", false);
    expect(till.action.has_value() && *till.action == EditorAction::TillForward, "t should map to till forward");

    KeyDispatch visual_till = dispatch_key_sequence(keybindings, "normal", pending, "T", false);
    expect(
        visual_till.action.has_value() && *visual_till.action == EditorAction::VisualTillForward,
        "T should map to visual till forward");

    KeyDispatch search = dispatch_key_sequence(keybindings, "normal", pending, "/", false);
    expect(
        search.action.has_value() && *search.action == EditorAction::EnterSearchMode,
        "/ should map to search mode");

    KeyDispatch search_prev = dispatch_key_sequence(keybindings, "normal", pending, "b", false);
    expect(
        search_prev.action.has_value() && *search_prev.action == EditorAction::SearchPrevious,
        "b should map to previous search result");

    KeyDispatch visual_search_next = dispatch_key_sequence(keybindings, "normal", pending, "N", false);
    expect(
        visual_search_next.action.has_value() && *visual_search_next.action == EditorAction::VisualSearchNext,
        "N should map to visual next search result");

    KeyDispatch visual_search_prev = dispatch_key_sequence(keybindings, "normal", pending, "B", false);
    expect(
        visual_search_prev.action.has_value() && *visual_search_prev.action == EditorAction::VisualSearchPrevious,
        "B should map to visual previous search result");

    KeyDispatch replace_char = dispatch_key_sequence(keybindings, "normal", pending, "r", false);
    expect(
        replace_char.action.has_value() && *replace_char.action == EditorAction::ReplaceChar,
        "r should map to replace char");

    KeyDispatch repeat = dispatch_key_sequence(keybindings, "normal", pending, ".", false);
    expect(
        repeat.action.has_value() && *repeat.action == EditorAction::RepeatLastCommand,
        ". should map to repeat last command");

    KeyDispatch redo = dispatch_key_sequence(keybindings, "normal", pending, "ctrl-r", false);
    expect(
        redo.action.has_value() && *redo.action == EditorAction::Redo,
        "ctrl-r should map to redo");

    KeyDispatch definition_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(definition_first.matched && definition_first.waiting_for_more, "g should wait for gd");
    KeyDispatch definition_second = dispatch_key_sequence(keybindings, "normal", pending, "d", false);
    expect(
        definition_second.action.has_value() && *definition_second.action == EditorAction::GoToDefinition,
        "gd should map to go to definition");

    KeyDispatch reverse_find_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(reverse_find_first.matched && reverse_find_first.waiting_for_more, "g should wait for gf backward find");
    KeyDispatch reverse_find_second = dispatch_key_sequence(keybindings, "normal", pending, "f", false);
    expect(
        reverse_find_second.action.has_value() && *reverse_find_second.action == EditorAction::FindBackward,
        "gf should map to backward find");

    KeyDispatch reverse_visual_find_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(reverse_visual_find_first.matched && reverse_visual_find_first.waiting_for_more, "g should wait for gF");
    KeyDispatch reverse_visual_find_second = dispatch_key_sequence(keybindings, "normal", pending, "F", false);
    expect(
        reverse_visual_find_second.action.has_value() && *reverse_visual_find_second.action == EditorAction::VisualFindBackward,
        "gF should map to visual backward find");

    KeyDispatch reverse_till_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(reverse_till_first.matched && reverse_till_first.waiting_for_more, "g should wait for gt");
    KeyDispatch reverse_till_second = dispatch_key_sequence(keybindings, "normal", pending, "t", false);
    expect(
        reverse_till_second.action.has_value() && *reverse_till_second.action == EditorAction::TillBackward,
        "gt should map to backward till");

    KeyDispatch reverse_visual_till_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(reverse_visual_till_first.matched && reverse_visual_till_first.waiting_for_more, "g should wait for gT");
    KeyDispatch reverse_visual_till_second = dispatch_key_sequence(keybindings, "normal", pending, "T", false);
    expect(
        reverse_visual_till_second.action.has_value() && *reverse_visual_till_second.action == EditorAction::VisualTillBackward,
        "gT should map to visual backward till");

    KeyDispatch file_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(file_first.matched && file_first.waiting_for_more, "g should wait for gp");
    KeyDispatch file_second = dispatch_key_sequence(keybindings, "normal", pending, "p", false);
    expect(
        file_second.action.has_value() && *file_second.action == EditorAction::GoToFileUnderCursor,
        "gp should map to go to file under cursor");

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

    KeyDispatch visual_top_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(visual_top_first.matched && visual_top_first.waiting_for_more, "g should wait for gG");
    KeyDispatch visual_top_second = dispatch_key_sequence(keybindings, "normal", pending, "G", false);
    expect(
        visual_top_second.action.has_value() && *visual_top_second.action == EditorAction::VisualGotoTop,
        "gG should map to visual goto top");

    KeyDispatch bottom_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(bottom_first.matched && bottom_first.waiting_for_more, "g should wait for ge");
    KeyDispatch bottom_second = dispatch_key_sequence(keybindings, "normal", pending, "e", false);
    expect(
        bottom_second.action.has_value() && *bottom_second.action == EditorAction::GotoBottom,
        "ge should map to goto bottom");

    KeyDispatch visual_bottom_first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(visual_bottom_first.matched && visual_bottom_first.waiting_for_more, "g should wait for gE");
    KeyDispatch visual_bottom_second = dispatch_key_sequence(keybindings, "normal", pending, "E", false);
    expect(
        visual_bottom_second.action.has_value() && *visual_bottom_second.action == EditorAction::VisualGotoBottom,
        "gE should map to visual goto bottom");

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

    KeyDispatch toggle_diags = dispatch_key_sequence(keybindings, "normal", pending, "ctrl-g", false);
    expect(
        toggle_diags.action.has_value() && *toggle_diags.action == EditorAction::ToggleDiagnosticsVisibility,
        "ctrl-g should toggle diagnostics visibility");

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

    KeyDispatch selection_start = dispatch_key_sequence(keybindings, "visual", pending, "o", false);
    expect(
        selection_start.action.has_value() && *selection_start.action == EditorAction::MoveToSelectionStart,
        "visual o should move to selection start");

    KeyDispatch selection_end = dispatch_key_sequence(keybindings, "visual", pending, "O", false);
    expect(
        selection_end.action.has_value() && *selection_end.action == EditorAction::MoveToSelectionEnd,
        "visual O should move to selection end");

    KeyDispatch select_all = dispatch_key_sequence(keybindings, "normal", pending, "%", false);
    expect(
        select_all.action.has_value() && *select_all.action == EditorAction::JumpToMatchingPair,
        "% should map to jump to matching pair");

    KeyDispatch visual_line_start = dispatch_key_sequence(keybindings, "normal", pending, ")", false);
    expect(
        visual_line_start.action.has_value() && *visual_line_start.action == EditorAction::VisualMoveLineStart,
        ") should map to visual line start");

    KeyDispatch visual_line_end = dispatch_key_sequence(keybindings, "normal", pending, "^", false);
    expect(
        visual_line_end.action.has_value() && *visual_line_end.action == EditorAction::VisualMoveLineEnd,
        "^ should map to visual line end");

    KeyDispatch visual_mode_home = dispatch_key_sequence(keybindings, "visual", pending, "home", false);
    expect(
        visual_mode_home.action.has_value() && *visual_mode_home.action == EditorAction::MoveLineStart,
        "visual home should map to line start");

    KeyDispatch visual_mode_shift_home = dispatch_key_sequence(keybindings, "visual", pending, "shift-home", false);
    expect(
        visual_mode_shift_home.action.has_value() && *visual_mode_shift_home.action == EditorAction::VisualMoveLineStart,
        "visual shift-home should map to visual line start");

    KeyDispatch visual_mode_end = dispatch_key_sequence(keybindings, "visual", pending, "end", false);
    expect(
        visual_mode_end.action.has_value() && *visual_mode_end.action == EditorAction::MoveLineEnd,
        "visual end should map to line end");

    KeyDispatch visual_mode_shift_end = dispatch_key_sequence(keybindings, "visual", pending, "shift-end", false);
    expect(
        visual_mode_shift_end.action.has_value() && *visual_mode_shift_end.action == EditorAction::VisualMoveLineEnd,
        "visual shift-end should map to visual line end");

    KeyDispatch search_insert = dispatch_key_sequence(keybindings, "search", pending, "x", true);
    expect(
        search_insert.action.has_value() && *search_insert.action == EditorAction::SearchInsert,
        "search printable binding");

    KeyDispatch search_history_previous = dispatch_key_sequence(keybindings, "search", pending, "up", false);
    expect(
        search_history_previous.action.has_value() &&
            *search_history_previous.action == EditorAction::SearchHistoryPrevious,
        "search up should browse previous history");

    KeyDispatch search_history_next = dispatch_key_sequence(keybindings, "search", pending, "down", false);
    expect(
        search_history_next.action.has_value() &&
            *search_history_next.action == EditorAction::SearchHistoryNext,
        "search down should browse next history");
}

void test_keybinding_hints() {
    KeyBindings keybindings = load_embedded_keybindings();

    std::vector<KeyHint> root_hints = key_hints_for_prefix(keybindings, "normal", {});
    const KeyHint *g_hint = find_key_hint(root_hints, "g");
    expect(g_hint != nullptr, "root hints should include g prefix");
    expect(g_hint->detail.find("d goto definition") != std::string::npos, "g hints should list gd");
    expect(g_hint->detail.find("k show hover") != std::string::npos, "g hints should list gk");

    const KeyHint *window_hint = find_key_hint(root_hints, "ctrl-w");
    expect(window_hint != nullptr, "root hints should include ctrl-w prefix");
    expect(window_hint->detail.find("s split horizontal") != std::string::npos, "ctrl-w hints should list split");
    expect(window_hint->detail.find("l focus window right") != std::string::npos, "ctrl-w hints should list focus");

    std::vector<KeyHint> g_hints = key_hints_for_prefix(keybindings, "normal", {"g"});
    expect(g_hints.size() >= 6, "g prefix should expose multiple completions");
    const KeyHint *definition_hint = find_key_hint(g_hints, "d");
    expect(definition_hint != nullptr, "g hints should include d");
    expect(definition_hint->detail == "goto definition", "gd hint should describe definition jump");

    const KeyHint *hover_hint = find_key_hint(g_hints, "k");
    expect(hover_hint != nullptr && hover_hint->detail == "show hover", "gk hint should describe hover");

    std::vector<KeyHint> no_hints = key_hints_for_prefix(keybindings, "insert", {"g"});
    expect(no_hints.empty(), "unknown insert prefix should have no hints");
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
        rc << "lua = init.lua\n";
        rc << "log = debug.log\n";
        rc << "syntax_config = syntax.json\n";
        rc << "syntax = cpp\n";
        rc << "right_justify_diagnostics = true\n";
        rc << "show_diagnostics_in_insert_mode = false\n";
        rc << "tabstop = 8\n";
        rc << "softtabstop = 0\n";
        rc << "expandtab = false\n";
        rc << "shiftwidth = 4\n";
        rc << "autoindent = false\n";
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
                  "      \"highlights_path\": \"queries/python/highlights.scm\",\n"
                  "      \"editor\": {\n"
                  "        \"shiftwidth\": 2,\n"
                  "        \"tabstop\": 8,\n"
                  "        \"softtabstop\": 2,\n"
                  "        \"expandtab\": true,\n"
                  "        \"autoindent\": true,\n"
                  "        \"show_diagnostics_in_insert_mode\": false\n"
                  "      }\n"
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
    {
        std::ofstream lua(medit_dir + "/init.lua");
        lua << "medit.set_status('lua ready')\n";
    }

    EditorConfig config = load_editor_config_from_path(config_dir + "/meditrc");
    expect(config.keybindings_path.has_value(), "config should resolve keybindings path");
    expect(config.colors_path.has_value(), "config should resolve colors path");
    expect(config.lsp_path.has_value(), "config should resolve lsp path");
    expect(config.log_path.has_value(), "config should resolve log path");
    expect(config.syntax_config_path.has_value(), "config should resolve syntax config path");
    expect(config.lua_path.has_value(), "config should resolve lua path");
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
    expect(config.lua_path->filename() == "init.lua", "config should use configured lua file");
    expect(config.lsp_servers.size() == 1, "config should parse lsp server rules");
    expect(config.syntax_languages.size() == 1, "config should parse syntax language rules");
    expect(config.syntax_languages[0].name == "python", "config should parse syntax language name");
    expect(config.syntax_languages[0].grammar_path.filename() == "libtree-sitter-python.so", "config should parse syntax grammar path");
    expect(config.syntax_languages[0].symbol_name == "tree_sitter_python", "config should parse syntax symbol");
    expect(config.syntax_languages[0].highlights_path.filename() == "highlights.scm", "config should parse syntax query path");
    expect(config.syntax_languages[0].editor.shiftwidth == 2, "config should parse syntax editor shiftwidth");
    expect(config.syntax_languages[0].editor.tabstop == 8, "config should parse syntax editor tabstop");
    expect(config.syntax_languages[0].editor.softtabstop == 2, "config should parse syntax editor softtabstop");
    expect(config.syntax_languages[0].editor.expandtab == true, "config should parse syntax editor expandtab");
    expect(config.syntax_languages[0].editor.autoindent == true, "config should parse syntax editor autoindent");
    expect(
        config.syntax_languages[0].editor.show_diagnostics_in_insert_mode == false,
        "config should parse syntax editor diagnostic visibility");
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
    expect(!config.show_diagnostics_in_insert_mode, "config should parse insert-mode diagnostic visibility");
    expect(config.tabstop == 8, "config should parse tabstop");
    expect(config.softtabstop == 0, "config should parse softtabstop");
    expect(!config.expandtab, "config should parse expandtab");
    expect(config.shiftwidth == 4, "config should parse shiftwidth");
    expect(!config.autoindent, "config should parse autoindent");
    expect(effective_shiftwidth(config, std::optional<std::string>("demo.py")) == 2, "syntax settings should override shiftwidth");
    expect(effective_tabstop(config, std::optional<std::string>("demo.py")) == 8, "syntax settings should override tabstop");
    expect(effective_softtabstop(config, std::optional<std::string>("demo.py")) == 2, "syntax settings should override softtabstop");
    expect(effective_expandtab(config, std::optional<std::string>("demo.py")), "syntax settings should override expandtab");
    expect(effective_autoindent(config, std::optional<std::string>("demo.py")), "syntax settings should override autoindent");
    expect(
        !effective_show_diagnostics_in_insert_mode(config, std::optional<std::string>("demo.py")),
        "syntax settings should override insert-mode diagnostic visibility");

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
    config.lsp_servers.push_back(
        {"python", "pyright-langserver --stdio", "python", {"*.py", "*.pyi", "*.pyw"}, {}});
    config.lsp_servers.push_back(
        {"json", "vscode-json-languageserver --stdio", "json", {"*.json", "*.jsonc"}, {}});

    expect(infer_language_id(config, std::optional<std::string>("test.py")) == "python", "python pattern should infer python");
    expect(infer_language_id(config, std::optional<std::string>("init.lua")) == "lua", "lua extension should infer lua");
    expect(infer_language_id(config, std::optional<std::string>("settings.json")) == "json", "json pattern should infer json");
    expect(infer_language_id(config, std::optional<std::string>("main.cpp")) == "cpp", "cpp fallback should infer cpp");
    expect(infer_language_id(config, std::optional<std::string>("notes.txt")) == "text", "unknown filename should infer text");
    expect(infer_language_id(config, std::optional<std::string>("dir/.pythonrc")) == "text", "unmatched basename should not infer language");

    EditorConfig fallback;
    fallback.syntax_name = "cpp";
    expect(infer_language_id(fallback, std::nullopt) == "cpp", "syntax fallback should be used when no file path exists");
}

void test_lua_runtime_registers_and_executes_command() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_lua_runtime";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::filesystem::path script_path = root / "init.lua";
    {
        std::ofstream script(script_path);
        script << "medit.register_command('hello', function()\n"
                  "  medit.set_status('hello from lua')\n"
                  "end)\n";
    }

    EditorState state;
    initialize_windows(state);
    std::string error_message;
    expect(state.lua.initialize(state, script_path, error_message), "lua runtime should initialize");
    expect(error_message.empty(), "lua runtime init should not set an error");
    std::vector<std::string> commands = state.lua.registered_commands();
    expect(commands.size() == 1 && commands[0] == "hello", "lua runtime should register commands from startup script");
    expect(state.lua.execute_command(state, "hello", "", error_message), "lua runtime should execute registered command");
    expect(state.status_message == "hello from lua", "lua command should be able to set editor status");
    state.lua.shutdown();
    std::filesystem::remove_all(root);
}

void test_lua_runtime_passes_command_argument() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_lua_runtime_args";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::filesystem::path script_path = root / "init.lua";
    {
        std::ofstream script(script_path);
        script << "medit.register_command('echo', function(argument)\n"
                  "  medit.set_status('arg=' .. argument)\n"
                  "end)\n";
    }

    EditorState state;
    initialize_windows(state);
    std::string error_message;
    expect(state.lua.initialize(state, script_path, error_message), "lua runtime should initialize for argument test");
    expect(error_message.empty(), "lua runtime init should not set an error for argument test");
    expect(state.lua.execute_command(state, "echo", "hello world", error_message), "lua runtime should pass command arguments");
    expect(state.status_message == "arg=hello world", "lua command should receive ex command arguments");
    state.lua.shutdown();
    std::filesystem::remove_all(root);
}

void test_lua_commands_are_top_level_ex_commands() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_lua_top_level_command";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::filesystem::path script_path = root / "init.lua";
    {
        std::ofstream script(script_path);
        script << "medit.register_command('hello', function()\n"
                  "  medit.set_status('hello from top level')\n"
                  "end)\n";
    }

    EditorState state;
    initialize_windows(state);
    std::string error_message;
    expect(state.lua.initialize(state, script_path, error_message), "lua runtime should initialize for top-level ex command test");
    expect(error_message.empty(), "lua runtime init should not set an error for top-level ex command test");

    state.enter_command_mode();
    state.command_buffer = utf8_to_u32("he");
    state.prompt_cursor = state.command_buffer.size();
    state.show_command_completion();
    expect(state.popup.visible, "command completion should show a popup for Lua commands");
    expect(state.popup.kind == PopupKind::Menu, "command completion should use a menu popup for Lua commands");

    bool saw_hello = false;
    for (const PopupMenuItem &item : state.popup.items) {
        if (item.label == "hello" && item.insert_text == "hello") {
            saw_hello = true;
            break;
        }
    }
    expect(saw_hello, "top-level command completion should include registered Lua commands");

    state.dismiss_popup();
    state.command_buffer = utf8_to_u32("hello");
    state.prompt_cursor = state.command_buffer.size();
    state.execute_command();
    expect(state.mode == Mode::Normal, "executing a top-level Lua command should return to normal mode");
    expect(state.status_message == "hello from top level", "top-level ex command should execute the registered Lua command");

    state.lua.shutdown();
    std::filesystem::remove_all(root);
}

void test_lua_runtime_sets_line_annotations() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_lua_line_annotations";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::filesystem::path script_path = root / "init.lua";
    {
        std::ofstream script(script_path);
        script << "medit.register_command('annotate', function()\n"
                  "  medit.set_line_annotations({\n"
                  "    {\n"
                  "      line = 0,\n"
                  "      text = 'preview',\n"
                  "      source = 'theme-preview',\n"
                  "      style = { foreground = 'color117', bold = true }\n"
                  "    }\n"
                  "  })\n"
                  "end)\n";
    }

    EditorState state;
    initialize_windows(state);
    expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha\nbeta")), "seed lua annotation buffer");
    std::string error_message;
    expect(state.lua.initialize(state, script_path, error_message), "lua runtime should initialize for line annotation test");
    expect(state.lua.execute_command(state, "annotate", "", error_message), "lua should set line annotations");
    expect(state.active_core().lua_annotations().size() == 1, "lua command should create one line annotation");
    expect(state.active_core().lua_annotations()[0].style_override.has_value(), "lua annotation should carry style override");

    state.lua.shutdown();
    std::filesystem::remove_all(root);
}

void test_lua_runtime_rejects_invalid_line_annotations() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_lua_line_annotations_invalid";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::filesystem::path script_path = root / "init.lua";
    {
        std::ofstream script(script_path);
        script << "medit.register_command('bad-annotate', function()\n"
                  "  medit.set_line_annotations({{ line = 99, text = 'oops' }})\n"
                  "end)\n";
    }

    EditorState state;
    initialize_windows(state);
    expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha")), "seed invalid lua annotation buffer");
    std::string error_message;
    expect(state.lua.initialize(state, script_path, error_message), "lua runtime should initialize for invalid annotation test");
    expect(!state.lua.execute_command(state, "bad-annotate", "", error_message), "invalid lua annotation should fail");
    expect(error_message.find("line out of range") != std::string::npos, "invalid annotation error should mention line range");

    state.lua.shutdown();
    std::filesystem::remove_all(root);
}

void test_lua_runtime_rejects_invalid_async_job_buffer() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_lua_async_invalid_buffer";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::filesystem::path script_path = root / "init.lua";
    {
        std::ofstream script(script_path);
        script << "medit.register_command('bad-job', function()\n"
                  "  medit.job_start({ command = \"printf hi\", buffer_id = 999999 })\n"
                  "end)\n";
    }

    EditorState state;
    initialize_windows(state);
    std::string error_message;
    expect(state.lua.initialize(state, script_path, error_message), "lua runtime should initialize for invalid async buffer test");
    expect(!state.lua.execute_command(state, "bad-job", "", error_message), "invalid async job buffer should fail");
    expect(error_message.find("buffer_id not found") != std::string::npos, "invalid async job buffer should mention missing buffer");

    state.lua.shutdown();
    std::filesystem::remove_all(root);
}

void test_special_buffers_and_panel_reuse() {
    EditorState state;
    initialize_windows(state);

    std::size_t first_id = state.ensure_named_special_buffer("build", EditorBufferKind::Output, false, false).id;
    std::size_t second_id = state.ensure_named_special_buffer("grep", EditorBufferKind::List, false, false).id;
    EditorBuffer *first = state.session.find_buffer_by_id(first_id);
    EditorBuffer *second = state.session.find_buffer_by_id(second_id);
    expect(first != nullptr, "first special buffer should exist");
    expect(second != nullptr, "second special buffer should exist");
    expect(first->kind == EditorBufferKind::Output, "first special buffer should keep requested kind");
    expect(!first->editable, "output buffer should be non-editable");
    expect(first->ephemeral, "output buffer should be ephemeral");
    expect(buffer_display_name(*first) == "build", "special buffer should use title as display name");

    std::size_t original_window_id = state.windows.active_window_id();
    state.show_buffer_in_panel(first_id, false);
    expect(state.panel.window_id.has_value(), "show_buffer_in_panel should create a panel window");
    std::size_t panel_window_id = *state.panel.window_id;
    expect(panel_window_id != original_window_id, "panel should use a different window from the editing window");
    expect(state.windows.window_count() == 2, "showing first panel buffer should create a split");
    expect(state.panel.visible, "showing panel buffer should mark the panel visible");
    expect(state.panel.buffer_id.has_value() && *state.panel.buffer_id == first_id, "panel should track its buffer");
    expect(state.windows.active_window_id() == original_window_id, "show_buffer_in_panel without focus should restore original focus");
    expect(state.window_buffer(panel_window_id).id == first_id, "panel should show the requested buffer");

    state.show_buffer_in_panel(second_id, false);
    expect(state.panel.window_id.has_value() && *state.panel.window_id == panel_window_id, "panel window should be reused");
    expect(state.windows.window_count() == 2, "reusing panel should not create more windows");
    expect(state.window_buffer(panel_window_id).id == second_id, "reused panel should swap to the new buffer");
    expect(state.panel.buffer_id.has_value() && *state.panel.buffer_id == second_id, "panel should update its tracked buffer");

    expect(state.focus_panel(), "panel should be focusable");
    expect(state.windows.active_window_id() == panel_window_id, "focus_panel should move focus into the panel");

    state.append_to_buffer(second_id, U"build line\n", false);
    expect(state.window_core(panel_window_id).cursor().row == 1, "panel should follow appended output by default");
    state.set_panel_follow_output(false);
    state.append_to_buffer(second_id, U"second line\n", false);
    expect(state.window_core(panel_window_id).cursor().row == 1, "panel follow should stop when disabled");

    expect(state.clear_panel(), "panel should be clearable");
    expect(buffer_text_utf8(state.window_buffer(panel_window_id)).empty(), "clearing panel should empty the panel buffer");
    expect(state.panel.follow_output, "clearing panel should re-enable follow output");

    expect(state.toggle_panel(), "toggle_panel should hide a visible panel");
    expect(!state.panel.visible, "toggle_panel should hide the panel");
    expect(state.windows.window_count() == 1, "hiding the panel should close its split");
    expect(state.toggle_panel(), "toggle_panel should restore a hidden panel with a buffer");
    expect(state.panel.visible, "toggle_panel should restore the panel");
    expect(state.panel.window_id.has_value(), "restored panel should have a window");

    expect(state.close_panel(true), "close_panel should close the panel when visible");
    expect(!state.panel.visible, "close_panel should leave the panel hidden");
    expect(state.panel.buffer_id.has_value() && *state.panel.buffer_id == second_id, "closing with preserve should keep the panel buffer");

    EditorBuffer &reused = state.ensure_named_special_buffer("build", EditorBufferKind::Output, false, false);
    expect(reused.id == first_id, "named special buffers should be reused by name");
}

void test_lua_async_job_streams_output_to_named_buffer() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_lua_async_job";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::filesystem::path script_path = root / "init.lua";
    {
        std::ofstream script(script_path);
        script << "medit.register_command('async-test', function()\n"
                  "  local buffer_id = medit.create_buffer('job-output', 'output')\n"
                  "  medit.clear_buffer(buffer_id)\n"
                  "  medit.job_start({\n"
                  "    command = \"printf 'hello from job\\\\n'\",\n"
                  "    buffer_id = buffer_id,\n"
                  "    on_exit = function(_, exit_code)\n"
                  "      medit.set_status('job exit=' .. exit_code)\n"
                  "    end\n"
                  "  })\n"
                  "end)\n";
    }

    EditorState state;
    initialize_windows(state);
    std::string error_message;
    expect(state.lua.initialize(state, script_path, error_message), "lua runtime should initialize for async job test");
    expect(error_message.empty(), "lua runtime init should not set an error for async job test");
    expect(state.lua.execute_command(state, "async-test", "", error_message), "lua async test command should execute");

    bool completed = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        state.lua.poll_async(state);
        if (state.status_message == "job exit=0") {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(completed, "async Lua job should finish and run on_exit callback");

    auto found = state.named_special_buffers.find("job-output");
    expect(found != state.named_special_buffers.end(), "async Lua job should create a named output buffer");
    EditorBuffer *buffer = state.session.find_buffer_by_id(found->second);
    expect(buffer != nullptr, "async Lua output buffer should exist");
    expect(buffer->kind == EditorBufferKind::Output, "async Lua output buffer should be an output buffer");
    expect_text(buffer->core, "hello from job\n", "async Lua job output should be appended to the output buffer");

    state.lua.shutdown();
    std::filesystem::remove_all(root);
}

void test_closing_buffer_clears_hidden_panel_buffer_reference() {
    EditorState state;
    initialize_windows(state);

    std::size_t panel_buffer_id = state.ensure_named_special_buffer("build", EditorBufferKind::Output, false, false).id;
    state.show_buffer_in_panel(panel_buffer_id, false);
    expect(state.close_panel(true), "closing panel should preserve its buffer before the deletion test");
    expect(state.panel.buffer_id.has_value() && *state.panel.buffer_id == panel_buffer_id, "hidden panel should remember its buffer");

    state.show_buffer_in_active_window(panel_buffer_id);
    state.handle_buffer_delete_command(true);

    expect(!state.panel.buffer_id.has_value(), "closing a hidden panel buffer should clear the preserved panel reference");
    expect(
        state.named_special_buffers.find("build") == state.named_special_buffers.end(),
        "closing a named special buffer should clear the named buffer lookup");
    expect_editor_state_sane(state, "closing hidden panel buffer");
}

void test_closing_buffer_detaches_async_job_buffer_reference() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_lua_async_detach";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::filesystem::path script_path = root / "init.lua";
    {
        std::ofstream script(script_path);
        script << "medit.register_command('async-detach-test', function()\n"
                  "  local buffer_id = medit.create_buffer('job-output', 'output')\n"
                  "  medit.clear_buffer(buffer_id)\n"
                  "  medit.job_start({\n"
                  "    command = \"sleep 0.05; printf 'late output\\\\n'\",\n"
                  "    buffer_id = buffer_id,\n"
                  "    on_exit = function(job_id, _)\n"
                  "      local job = medit.job_status(job_id)\n"
                  "      if job ~= nil and job.buffer_id == nil then\n"
                  "        medit.set_status('buffer detached')\n"
                  "      else\n"
                  "        medit.set_status('buffer still attached')\n"
                  "      end\n"
                  "    end\n"
                  "  })\n"
                  "end)\n";
    }

    EditorState state;
    initialize_windows(state);
    std::string error_message;
    expect(state.lua.initialize(state, script_path, error_message), "lua runtime should initialize for async detach test");
    expect(error_message.empty(), "lua runtime init should not set an error for async detach test");
    expect(state.lua.execute_command(state, "async-detach-test", "", error_message), "lua async detach test command should execute");

    auto found = state.named_special_buffers.find("job-output");
    expect(found != state.named_special_buffers.end(), "async detach test should create an output buffer");
    state.show_buffer_in_active_window(found->second);
    state.handle_buffer_delete_command(true);

    bool detached = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        state.lua.poll_async(state);
        if (state.status_message == "buffer detached") {
            detached = true;
            break;
        }
        if (state.status_message == "buffer still attached") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    expect(detached, "closing a buffer should detach async jobs from the dead buffer id");
    expect(
        state.named_special_buffers.find("job-output") == state.named_special_buffers.end(),
        "closing the async output buffer should clear the named buffer lookup");
    expect_text(state.active_buffer().core, "", "async output from a closed buffer should not leak into the replacement buffer");

    state.lua.shutdown();
    std::filesystem::remove_all(root);
}

void test_invalid_buffer_ids_do_not_corrupt_windows_or_panel() {
    EditorState state;
    initialize_windows(state);

    const std::size_t original_buffer_id = state.active_buffer().id;
    const std::size_t original_window_id = state.windows.active_window_id();

    state.show_buffer_in_active_window(999999, true);
    expect(state.active_window().buffer_id == original_buffer_id, "invalid active-window buffer id should be ignored");
    expect_editor_state_sane(state, "invalid active-window buffer id");

    state.show_buffer_in_panel(999999, false);
    expect(!state.panel.window_id.has_value(), "invalid panel buffer id should not create a panel window");
    expect(!state.panel.buffer_id.has_value(), "invalid panel buffer id should not be preserved");
    expect(state.windows.active_window_id() == original_window_id, "invalid panel buffer id should not change focus");
    expect_editor_state_sane(state, "invalid panel buffer id");
}

void test_popup_dismisses_when_buffer_context_changes() {
    EditorState state;
    initialize_windows(state);
    expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha")), "seed popup context test");

    show_menu_popup(state, "Completion", {PopupMenuItem{"beta", {}, "beta", std::nullopt}});
    expect(state.popup.visible, "completion popup should start visible");

    std::size_t second_id = state.session.new_buffer(true).id;
    state.show_buffer_in_active_window(second_id);

    expect(!state.popup.visible, "buffer-bound popup should dismiss when switching to another buffer");
    expect_editor_state_sane(state, "popup dismissed on buffer switch");
}

void test_popup_dismisses_when_buffer_is_closed() {
    EditorState state;
    initialize_windows(state);
    expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha")), "seed popup close test");

    show_menu_popup(state, "Completion", {PopupMenuItem{"beta", {}, "beta", std::nullopt}});
    expect(state.popup.visible, "completion popup should start visible before close");

    state.handle_buffer_delete_command(true);

    expect(!state.popup.visible, "closing the owning buffer should dismiss its popup");
    expect_editor_state_sane(state, "popup dismissed on buffer close");
}

void test_command_execution_preserves_command_status() {
    EditorState state;
    initialize_windows(state);
    state.enter_command_mode();
    state.command_buffer = utf8_to_u32("buffers");
    state.prompt_cursor = state.command_buffer.size();
    state.execute_command();
    expect(state.mode == Mode::Normal, "executing a command should return to normal mode");
    expect(state.status_message != "NORMAL", "command result status should not be overwritten on exit");
}

void test_tree_sitter_health_summary_for_empty_config() {
    EditorConfig config;
    std::string summary = tree_sitter_health_summary(config);
    expect(summary.contains("configured languages: 0"), "tree-sitter health should report zero configured languages");
    expect(summary.contains("syntax config: (default/none)"), "tree-sitter health should show missing syntax config");
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
    EditorConfig config;
    SyntaxSelection detected_cpp = resolve_syntax_selection(config, std::optional<std::string>("sample.cpp"));
    expect(detected_cpp.engine == SyntaxEngine::None, "cpp extension should not auto-detect syntax without configured tree-sitter language");

    SyntaxSelection detected_none = resolve_syntax_selection(config, std::optional<std::string>("notes.txt"));
    expect(detected_none.engine == SyntaxEngine::None, "non-code file should not auto-detect syntax");

    EditorConfig explicit_python;
    explicit_python.syntax_languages.push_back({"python", {"*.py", ".pythonrc"}, "python.so", "tree_sitter_python", "highlights.scm", {}});
    explicit_python.syntax_name = "python";
    SyntaxSelection configured = resolve_syntax_selection(explicit_python, std::optional<std::string>("notes.txt"));
    expect(configured.engine == SyntaxEngine::TreeSitter && configured.language_name == "python", "named tree-sitter syntax should resolve");

    std::vector<std::u32string> lines = {
        utf8_to_u32("print('hello')"),
    };
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

void test_popup_selection_accept_tokens() {
    expect(popup_selection_accept_token("tab"), "tab should accept popup selections");
    expect(popup_selection_accept_token("enter"), "enter should still accept popup selections");
    expect(!popup_selection_accept_token("shift-tab"), "shift-tab should remain a navigation key");
    expect(!popup_selection_accept_token("down"), "arrow navigation should not accept popup selections");
}

void expect_editor_state_sane(const EditorState &state, const std::string &context) {
    expect(state.windows.window_count() >= 1, context + ": editor should always keep at least one window");
    expect(
        state.session.find_buffer_by_id(state.windows.active_window()->buffer_id) != nullptr,
        context + ": active window should reference a live buffer");
    if (state.mode == Mode::Command) {
        expect(state.prompt_cursor <= state.command_buffer.size(), context + ": command prompt cursor should stay in range");
    }
    if (state.mode == Mode::Search) {
        expect(state.prompt_cursor <= state.search_buffer.size(), context + ": search prompt cursor should stay in range");
    }
    if (state.panel.window_id.has_value()) {
        expect(state.windows.find_window(*state.panel.window_id) != nullptr, context + ": panel window id should stay valid");
    }
    if (state.panel.buffer_id.has_value()) {
        expect(state.session.find_buffer_by_id(*state.panel.buffer_id) != nullptr, context + ": panel buffer id should stay valid");
    }
}

void test_malformed_key_sequence_fuzz() {
    ScopedTestScreen screen;
    std::mt19937 rng(0x5eed1234u);
    const std::array<InjectedKeyEvent, 10> special_pool{{
        {KEY_UP, true},
        {KEY_DOWN, true},
        {KEY_LEFT, true},
        {KEY_RIGHT, true},
        {KEY_HOME, true},
        {KEY_END, true},
        {KEY_PPAGE, true},
        {KEY_NPAGE, true},
        {KEY_BTAB, true},
        {KEY_DC, true},
    }};
    const std::string printable_pool = "[;12345~OABCDrxyz:/?%()";
    const std::array<std::vector<InjectedKeyEvent>, 8> corpus{{
        {{27, false}, {'[', false}},
        {{27, false}, {'[', false}, {'1', false}, {';', false}},
        {{27, false}, {'[', false}, {'1', false}, {';', false}, {'2', false}, {'D', false}},
        {{27, false}, {'[', false}, {'9', false}, {'9', false}, {'~', false}},
        {{27, false}, {'O', false}, {'H', false}},
        {{27, false}, {'[', false}, {KEY_LEFT, true}},
        {{27, false}, {KEY_UP, true}},
        {{27, false}, {KEY_DOWN, true}},
    }};

    for (const auto &events : corpus) {
        EditorState state;
        initialize_windows(state);
        expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha\nbeta\ngamma")), "seed fuzz buffer");
        handle_test_input_sequence(state, events);
        expect_editor_state_sane(state, "malformed key corpus");
    }

    for (int iteration = 0; iteration < 5000; ++iteration) {
        EditorState state;
        initialize_windows(state);
        expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha\nbeta\ngamma")), "seed fuzz buffer");

        int mode_case = static_cast<int>(rng() % 4);
        if (mode_case == 1) {
            state.enter_insert_mode();
        } else if (mode_case == 2) {
            state.enter_command_mode();
        } else if (mode_case == 3) {
            state.enter_search_mode();
        }
        if (rng() % 7 == 0) {
            state.pending.motion = PendingMotion::FindForward;
            state.pending.motion_repeat_count = 1;
        }

        std::size_t event_count = 1 + (rng() % 8);
        std::vector<InjectedKeyEvent> events;
        events.reserve(event_count);
        for (std::size_t i = 0; i < event_count; ++i) {
            if (rng() % 5 == 0) {
                events.push_back(special_pool[rng() % special_pool.size()]);
                continue;
            }
            if (rng() % 4 == 0) {
                events.push_back({27, false});
                continue;
            }
            if (rng() % 6 == 0) {
                events.push_back({static_cast<wint_t>(1 + (rng() % 26)), false});
                continue;
            }
            events.push_back({static_cast<wint_t>(printable_pool[rng() % printable_pool.size()]), false});
        }

        handle_test_input_sequence(state, events);
        expect_editor_state_sane(state, "malformed key fuzz iteration " + std::to_string(iteration));
    }
}

void test_recorded_input_corpus_if_configured() {
    const char *corpus_dir_value = std::getenv("MEDIT_INPUT_CORPUS_DIR");
    if (corpus_dir_value == nullptr || *corpus_dir_value == '\0') {
        return;
    }

    std::filesystem::path corpus_dir = corpus_dir_value;
    if (!std::filesystem::exists(corpus_dir)) {
        return;
    }

    ScopedTestScreen screen;
    std::vector<std::filesystem::path> corpus_files;
    for (const auto &entry : std::filesystem::directory_iterator(corpus_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".keys") {
            corpus_files.push_back(entry.path());
        }
    }
    std::sort(corpus_files.begin(), corpus_files.end());

    for (const std::filesystem::path &path : corpus_files) {
        std::ifstream input(path);
        expect(static_cast<bool>(input), "could not open corpus file: " + path.string());

        std::vector<InjectedKeyEvent> events;
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            std::istringstream parser(line);
            char kind = '\0';
            long long raw_key = 0;
            parser >> kind >> raw_key;
            expect(
                parser && (kind == 'N' || kind == 'S') && raw_key >= 0,
                "invalid corpus line in " + path.string() + ": " + line);
            events.push_back({static_cast<wint_t>(raw_key), kind == 'S'});
        }

        EditorState state;
        initialize_windows(state);
        expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha\nbeta\ngamma")), "seed corpus replay buffer");
        handle_test_input_sequence(state, events);
        expect_editor_state_sane(state, "recorded corpus replay " + path.string());
    }
}

void test_edit_command_file_completion() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "medit_edit_completion";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "dir");
    {
        std::ofstream(root / "alpha.txt") << "alpha\n";
    }
    {
        std::ofstream(root / "beta.txt") << "beta\n";
    }

    std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(root);

    std::optional<EditFileCompletionResult> filtered = complete_edit_file_command("e a");
    expect(filtered.has_value(), "edit completion should resolve for the :e command");
    expect(filtered->initial_filter == "a", "edit completion should keep the current path fragment as the filter");
    expect(filtered->items.size() == 3, "edit completion should list directory entries before popup filtering");
    const PopupMenuItem &alpha_item = filtered->items[1];
    expect(alpha_item.label == "alpha.txt", "edit completion should include matching file names");
    expect(alpha_item.insert_text == "e alpha.txt", "edit completion should replace the command buffer with the completed path");

    std::optional<EditFileCompletionResult> unfiltered = complete_edit_file_command("e ");
    expect(unfiltered.has_value(), "edit completion should activate after the command name and a space");
    expect(!unfiltered->items.empty(), "edit completion should list directory entries");
    expect(unfiltered->items[0].label == "dir/", "edit completion should sort directories before files");

    std::optional<EditFileCompletionResult> nested = complete_edit_file_command("e dir/");
    expect(nested.has_value(), "edit completion should support nested directory prefixes");
    expect(nested->initial_filter == "dir/", "nested completion should keep the typed directory prefix");

    std::filesystem::current_path(previous);
    std::filesystem::remove_all(root);
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

void test_lsp_launch_pipe_cleanup() {
#if defined(__unix__) || defined(__APPLE__)
    std::array<int, 2> stdin_pipe{-1, -1};
    std::array<int, 2> stdout_pipe{-1, -1};
    std::array<int, 2> stderr_pipe{-1, -1};
    expect(pipe(stdin_pipe.data()) == 0, "stdin pipe should open");
    expect(pipe(stdout_pipe.data()) == 0, "stdout pipe should open");
    expect(pipe(stderr_pipe.data()) == 0, "stderr pipe should open");

    close_lsp_launch_pipes(stdin_pipe, stdout_pipe, stderr_pipe);

    for (int fd : {stdin_pipe[0], stdin_pipe[1], stdout_pipe[0], stdout_pipe[1], stderr_pipe[0], stderr_pipe[1]}) {
        expect(fd == -1, "cleanup helper should poison closed pipe descriptors");
    }

    errno = 0;
    expect(fcntl(stdin_pipe[0], F_GETFD) == -1 && errno == EBADF, "closed stdin read end should report EBADF");
    errno = 0;
    expect(fcntl(stdout_pipe[1], F_GETFD) == -1 && errno == EBADF, "closed stdout write end should report EBADF");
    errno = 0;
    expect(fcntl(stderr_pipe[0], F_GETFD) == -1 && errno == EBADF, "closed stderr read end should report EBADF");
#endif
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
    expect(core.diagnostics()[0].range.start.row == 0 && core.diagnostics()[0].range.start.column == 0, "open diagnostic start should map to live document");

    core.move_line_end();
    core.insert_codepoint(U'x');
    core.insert_codepoint(U'y');
    core.insert_codepoint(U'z');
    drive_runtime(30);
    expect(core.diagnostics().size() == 1, "lsp roundtrip should update diagnostics after change");
    expect(
        u32_to_utf8(core.diagnostics()[0].message) == "full:abcxyz",
        "lsp changed diagnostic message should reflect coalesced document text");
    expect(core.diagnostics()[0].range.end.row == 0 && core.diagnostics()[0].range.end.column == 1, "changed diagnostic range should still map on live text");

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

    request.type = ServiceRequestType::Hover;
    runtime.dispatch_service_request(request);

    bool saw_hover = false;
    for (int i = 0; i < 30 && !saw_hover; ++i) {
        runtime.poll_services();
        for (const ServiceEvent &event : runtime.take_service_events()) {
            if (!event.command || event.command->type != EditorCommandType::ShowPopup) {
                continue;
            }
            expect(event.command->title == "Hover", "hover should use popup title");
            expect(event.command->message == "hover:0:0", "hover should preserve hover contents");
            saw_hover = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(saw_hover, "lsp roundtrip should produce hover popup content");

    request.type = ServiceRequestType::Completion;
    request.document_version = core.document_version();
    runtime.dispatch_service_request(request);

    bool saw_completion = false;
    for (int i = 0; i < 30 && !saw_completion; ++i) {
        runtime.poll_services();
        for (const ServiceEvent &event : runtime.take_service_events()) {
            if (!event.command || event.command->type != EditorCommandType::ShowPopup) {
                continue;
            }
            if (event.command->popup_kind != PopupKind::Menu) {
                continue;
            }
            expect(event.command->title == "Completion", "completion should use popup title");
            expect(event.command->popup_items.size() == 2, "completion should include fake completion items");
            expect(event.command->popup_items[0].label == "completeOne", "completion should preserve first label");
            expect(event.command->popup_items[0].insert_text == "completeOne", "completion should preserve insert text");
            saw_completion = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(saw_completion, "lsp roundtrip should produce completion popup content");

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
        EditorCommand recorded;
        recorded.type = EditorCommandType::SetStatusMessage;
        recorded.message = "recorded";
        received_events.push_back(event);
        queued_events.push_back(
            {ServiceEventType::Notification,
             name(),
             event.type == EditorEventType::DocumentChanged ? "document_changed" : "editor_event",
             recorded,
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

    std::string status_summary() const override {
        return "recording\nstate: ready";
    }

    bool started = false;
    bool stopped = false;
    std::vector<EditorEvent> received_events;
    std::vector<ServiceEvent> queued_events;

  private:
    std::optional<int> interval_ms_;
};

class QueuedService : public EditorService {
  public:
    std::string name() const override {
        return "queued";
    }

    void start() override {}
    void stop() override {}
    void handle_editor_event(const EditorEvent &) override {}

    std::vector<ServiceEvent> poll() override {
        std::vector<ServiceEvent> events = std::move(queued_events);
        queued_events.clear();
        return events;
    }

    std::vector<ServiceEvent> queued_events;
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
    expect(runtime.status_summary().contains("recording"), "runtime status should include service name");
    expect(runtime.status_summary().contains("state: ready"), "runtime status should include service summary");

    runtime.stop_services();
    expect(!runtime.started(), "runtime stop should clear started state");
    expect(service_ptr->stopped, "runtime should stop registered services");
    std::vector<ServiceEvent> stopped_events = runtime.take_service_events();
    expect(stopped_events.size() == 1, "stopping runtime should emit one lifecycle event");
    expect(stopped_events[0].type == ServiceEventType::ServiceStopped, "runtime should emit service stopped");
}

void test_stale_service_hover_popup_is_dropped() {
    EditorState state;
    initialize_windows(state);
    expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha")), "seed hover stale-response test");

    auto service = std::make_unique<QueuedService>();
    QueuedService *service_ptr = service.get();
    state.runtime.add_service(std::move(service));
    state.runtime.start_services();
    state.runtime.take_service_events();

    EditorCommand command;
    command.type = EditorCommandType::ShowPopup;
    command.title = "Hover";
    command.message = "stale hover";
    command.document_uri = state.active_core().document_uri();
    service_ptr->queued_events.push_back(
        {ServiceEventType::Notification,
         service_ptr->name(),
         "hover",
         command,
         state.active_core().document_uri(),
         state.active_core().document_version() + 1,
         std::nullopt,
         U""});

    state.runtime.poll_services();
    state.handle_service_events();

    expect(!state.popup.visible, "stale hover popup should be dropped on version mismatch");
    expect(state.status_message != "stale hover", "stale hover popup should not overwrite editor status");
    state.runtime.stop_services();
}

void test_stale_service_selection_range_is_dropped() {
    EditorState state;
    initialize_windows(state);
    expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha beta")), "seed selection stale-response test");

    auto service = std::make_unique<QueuedService>();
    QueuedService *service_ptr = service.get();
    state.runtime.add_service(std::move(service));
    state.runtime.start_services();
    state.runtime.take_service_events();

    EditorCommand command;
    command.type = EditorCommandType::SetSelectionRange;
    command.document_uri = state.active_core().document_uri();
    command.selection_range = Range{{0, 0}, {0, 5}};
    command.selection_ranges = {*command.selection_range};
    command.position = Position{0, 0};
    service_ptr->queued_events.push_back(
        {ServiceEventType::Notification,
         service_ptr->name(),
         "selection_range",
         command,
         state.active_core().document_uri(),
         state.active_core().document_version() + 1,
         std::nullopt,
         U""});

    state.runtime.poll_services();
    state.handle_service_events();

    expect(state.mode == Mode::Normal, "stale selection-range response should not switch the editor into visual mode");
    expect(
        !state.displayed_selection_range(state.windows.active_window_id()).has_value(),
        "stale selection-range response should not install a selection");
    state.runtime.stop_services();
}

void test_stale_service_diagnostics_are_dropped() {
    EditorState state;
    initialize_windows(state);
    expect(state.active_core().insert_text({0, 0}, utf8_to_u32("alpha")), "seed diagnostics stale-response test");

    auto service = std::make_unique<QueuedService>();
    QueuedService *service_ptr = service.get();
    state.runtime.add_service(std::move(service));
    state.runtime.start_services();
    state.runtime.take_service_events();

    EditorCommand command;
    command.type = EditorCommandType::SetDiagnostics;
    command.document_uri = state.active_core().document_uri();
    command.diagnostics = {Diagnostic{{{0, 0}, {0, 1}}, DiagnosticSeverity::Error, "queued", utf8_to_u32("stale diagnostic")}};
    service_ptr->queued_events.push_back(
        {ServiceEventType::Notification,
         service_ptr->name(),
         "publishDiagnostics",
         command,
         state.active_core().document_uri(),
         state.active_core().document_version() + 1,
         std::nullopt,
         U""});

    state.runtime.poll_services();
    state.handle_service_events();

    expect(state.active_core().diagnostics().empty(), "stale diagnostics should be dropped on version mismatch");
    state.runtime.stop_services();
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

void test_editor_session_open_missing_startup_file() {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "medit_startup_missing_file";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    std::filesystem::path path = temp_dir / "missing.txt";
    EditorSession session;
    EditorBuffer *buffer = session.open_file(path.string(), true);
    expect(buffer != nullptr, "missing startup file should open as a named empty buffer");
    expect(buffer->core.file_path().has_value(), "missing startup file should keep its path");
    expect(*buffer->core.file_path() == path.string(), "missing startup file should keep requested file path");
    expect_text(buffer->core, "", "missing startup file buffer should start empty");

    std::filesystem::remove_all(temp_dir);
}

void test_window_manager_split_focus_and_close() {
    WindowManager windows(10);
    expect(windows.window_count() == 1, "window manager should start with one window");
    expect(windows.active_window() != nullptr && windows.active_window()->buffer_id == 10, "initial window buffer id");

    expect(windows.split_active(WindowSplitDirection::Vertical), "vertical split should succeed");
    expect(windows.window_count() == 2, "vertical split should add a second window");
    expect(windows.active_window() != nullptr && windows.active_window()->buffer_id == 10, "split window should show same buffer");

    auto rects = windows.layout_rects(24, 80, 2);
    expect(rects.size() == 2, "split should produce two layout rects");
    expect(windows.focus_direction(WindowMoveDirection::Left, 24, 80, 2), "focus left should succeed");
    std::size_t left_window = windows.active_window_id();
    expect(windows.focus_direction(WindowMoveDirection::Right, 24, 80, 2), "focus right should succeed");
    expect(windows.active_window_id() != left_window, "focus right should move to other window");

    expect(windows.split_active(WindowSplitDirection::Horizontal), "horizontal split should succeed");
    expect(windows.window_count() == 3, "horizontal split should add a third window");

    std::size_t current_window = windows.active_window_id();
    expect(windows.close_active(), "closing non-last window should succeed");
    expect(windows.window_count() == 2, "close active should remove one window");
    expect(windows.find_window(current_window) == nullptr, "closed window should be removed");

    expect(windows.close_others(), "close others should succeed");
    expect(windows.window_count() == 1, "close others should leave one window");
}

}  // namespace

int main() {
    try {
        test_insert_unicode_and_undo();
        test_newline_backspace_and_join();
        test_insert_mode_soft_tab_and_shift_tab();
        test_autoindent_newline();
        test_autoindent_open_line_below();
        test_autoindent_open_line_above();
        test_open_line_below_at_eof_then_newline();
        test_visual_range_semantics_and_delete();
        test_linewise_selection_range_and_delete();
        test_linewise_delete_and_paste();
        test_yank_and_paste();
        test_matching_pair_navigation();
        test_indent_and_outdent_lines();
        test_substitute_regex();
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
        test_lua_annotations_storage_and_events();
        test_diagnostics_follow_document_switches();
        test_editor_command_entry_points();
        test_compound_edit_undo();
        test_keybinding_dispatch();
        test_keybinding_hints();
        test_config_file_selects_keybindings_and_colors();
        test_lsp_config_rejects_duplicate_patterns();
        test_infer_language_id();
        test_lua_runtime_registers_and_executes_command();
        test_lua_runtime_passes_command_argument();
        test_lua_commands_are_top_level_ex_commands();
        test_lua_runtime_sets_line_annotations();
        test_lua_runtime_rejects_invalid_line_annotations();
        test_lua_runtime_rejects_invalid_async_job_buffer();
        test_special_buffers_and_panel_reuse();
        test_lua_async_job_streams_output_to_named_buffer();
        test_closing_buffer_clears_hidden_panel_buffer_reference();
        test_closing_buffer_detaches_async_job_buffer_reference();
        test_invalid_buffer_ids_do_not_corrupt_windows_or_panel();
        test_popup_dismisses_when_buffer_context_changes();
        test_popup_dismisses_when_buffer_is_closed();
        test_command_execution_preserves_command_status();
        test_tree_sitter_health_summary_for_empty_config();
        test_infer_workspace_root();
        test_process_utils_detect_missing_executables();
        test_cpp_syntax_highlighting();
        test_file_uri_normalization();
        test_string_utilities();
        test_popup_selection_accept_tokens();
        test_malformed_key_sequence_fuzz();
        test_recorded_input_corpus_if_configured();
        test_edit_command_file_completion();
        test_lsp_message_framing();
        test_lsp_launch_pipe_cleanup();
        test_lsp_service_roundtrip();
        test_lsp_service_reports_startup_failures();
        test_editor_runtime_service_boundary();
        test_stale_service_hover_popup_is_dropped();
        test_stale_service_selection_range_is_dropped();
        test_stale_service_diagnostics_are_dropped();
        test_editor_runtime_idle_timeout();
        test_editor_session_buffers_and_clipboard();
        test_editor_session_shared_file_clipboard();
        test_editor_session_open_and_close_rules();
        test_editor_session_open_missing_file_creates_named_buffer();
        test_editor_session_open_multiple_files();
        test_editor_session_open_missing_startup_file();
        test_window_manager_split_focus_and_close();
    } catch (const std::exception &error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
