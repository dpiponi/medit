#include "config.hpp"
#include "editor_core.hpp"
#include "keybindings.hpp"
#include "theme.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ncursesw/curses.h>
#include <stdexcept>
#include <string>
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

void test_keybinding_dispatch() {
    KeyBindings keybindings = load_embedded_keybindings();
    std::vector<std::string> pending;

    KeyDispatch first = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(first.matched && first.waiting_for_more && !first.action.has_value(), "g should wait for gg");

    KeyDispatch second = dispatch_key_sequence(keybindings, "normal", pending, "g", false);
    expect(second.action.has_value() && *second.action == EditorAction::GotoTop, "gg should map to goto top");

    KeyDispatch printable = dispatch_key_sequence(keybindings, "insert", pending, "x", true);
    expect(printable.action.has_value() && *printable.action == EditorAction::SelfInsert, "insert printable binding");

    KeyDispatch special = dispatch_key_sequence(keybindings, "normal", pending, "pagedown", false);
    expect(special.action.has_value() && *special.action == EditorAction::PageDown, "pagedown binding");

    KeyDispatch linewise = dispatch_key_sequence(keybindings, "normal", pending, "V", false);
    expect(
        linewise.action.has_value() && *linewise.action == EditorAction::EnterVisualLineMode,
        "V should map to linewise visual mode");

    KeyDispatch find = dispatch_key_sequence(keybindings, "normal", pending, "f", false);
    expect(find.action.has_value() && *find.action == EditorAction::FindForward, "f should map to find forward");

    KeyDispatch inner_first = dispatch_key_sequence(keybindings, "visual", pending, "i", false);
    expect(inner_first.matched && inner_first.waiting_for_more, "visual i should wait for iw");
    KeyDispatch inner_second = dispatch_key_sequence(keybindings, "visual", pending, "w", false);
    expect(
        inner_second.action.has_value() && *inner_second.action == EditorAction::SelectInnerWord,
        "visual iw should map to select inner word");
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
    }
    {
        std::ofstream keys(medit_dir + "/custom-keys.json");
        keys << "{\n"
                "  \"normal\": { \"z\": \"undo\" },\n"
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
    expect(
        config.keybindings_path->filename() == "custom-keys.json",
        "config should use configured keybindings file");
    expect(config.colors_path->filename() == "amber.json", "config should use configured colors file");

    KeyBindings keybindings = load_keybindings(config);
    std::vector<std::string> pending;
    KeyDispatch dispatch = dispatch_key_sequence(keybindings, "normal", pending, "z", false);
    expect(dispatch.action.has_value() && *dispatch.action == EditorAction::Undo, "custom keybinding should load");

    Theme theme = load_theme(config);
    TextStyle line_number = theme_style(theme, StyleRole::LineNumber);
    expect(line_number.foreground == COLOR_YELLOW, "custom color theme should load selected file");

    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    try {
        test_insert_unicode_and_undo();
        test_newline_backspace_and_join();
        test_visual_range_semantics_and_delete();
        test_linewise_selection_range_and_delete();
        test_linewise_delete_and_paste();
        test_yank_and_paste();
        test_replace_selection_with_yank();
        test_file_io_and_dirty_tracking();
        test_navigation();
        test_find_and_till_character_motions();
        test_word_object_ranges();
        test_extend_selection_to_word_object();
        test_generic_range_edit_api();
        test_text_edit_transactions();
        test_text_edit_transaction_rejects_overlaps();
        test_document_version_is_monotonic();
        test_unicode_position_conversions();
        test_keybinding_dispatch();
        test_config_file_selects_keybindings_and_colors();
    } catch (const std::exception &error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
