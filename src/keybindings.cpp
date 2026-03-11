#include "keybindings.hpp"

#include "json.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char *kEmbeddedDefaultKeybindings = R"json(
{
  "normal": {
    "left": "move_left",
    "right": "move_right",
    "up": "move_up",
    "down": "move_down",
    "h": "move_left",
    "j": "move_down",
    "k": "move_up",
    "l": "move_right",
    "f": "find_forward",
    "F": "find_backward",
    "t": "till_forward",
    "T": "till_backward",
    "0": "move_line_start",
    "$": "move_line_end",
    "i": "enter_insert_mode",
    "a": "append_after_cursor",
    "v": "enter_visual_mode",
    "V": "enter_visual_line_mode",
    "A": "append_line_end_insert",
    "I": "insert_line_start_insert",
    "o": "open_line_below",
    "O": "open_line_above",
    "x": "delete_char",
    "u": "undo",
    "r": "redo",
    "p": "paste_after",
    "P": "paste_before",
    "g g": "goto_top",
    "G": "goto_bottom",
    "/": "enter_search_mode",
    ":": "enter_command_mode",
    "n": "search_next",
    "b": "search_previous",
    "] d": "next_diagnostic",
    "[ d": "previous_diagnostic",
    "g x": "toggle_diagnostics_panel",
    "d d": "delete_line",
    "ctrl-d": "half_page_down",
    "ctrl-u": "half_page_up",
    "ctrl-z": "suspend",
    "pageup": "page_up",
    "pagedown": "page_down"
  },
  "visual": {
    "left": "move_left",
    "right": "move_right",
    "up": "move_up",
    "down": "move_down",
    "h": "move_left",
    "j": "move_down",
    "k": "move_up",
    "l": "move_right",
    "f": "find_forward",
    "F": "find_backward",
    "t": "till_forward",
    "T": "till_backward",
    "i w": "select_inner_word",
    "a w": "select_around_word",
    "0": "move_line_start",
    "$": "move_line_end",
    "g g": "goto_top",
    "G": "goto_bottom",
    "n": "search_next",
    "b": "search_previous",
    "] d": "next_diagnostic",
    "[ d": "previous_diagnostic",
    "c": "change_selection",
    "d": "delete_selection",
    "y": "yank_selection",
    "p": "replace_selection_with_yank",
    "P": "replace_selection_with_yank",
    "v": "enter_normal_mode",
    "V": "enter_visual_line_mode",
    "esc": "enter_normal_mode",
    "ctrl-d": "half_page_down",
    "ctrl-u": "half_page_up",
    "ctrl-z": "suspend",
    "pageup": "page_up",
    "pagedown": "page_down"
  },
  "insert": {
    "esc": "enter_normal_mode",
    "enter": "insert_newline",
    "backspace": "backspace",
    "left": "move_left",
    "right": "move_right",
    "up": "move_up",
    "down": "move_down",
    "printable": "self_insert"
  },
  "command": {
    "esc": "enter_normal_mode",
    "enter": "command_execute",
    "backspace": "command_backspace",
    "printable": "command_insert"
  },
  "search": {
    "esc": "enter_normal_mode",
    "enter": "search_execute",
    "backspace": "search_backspace",
    "printable": "search_insert"
  }
}
)json";

std::vector<std::string> split_key_sequence(const std::string &spec) {
    std::vector<std::string> tokens;
    std::istringstream parts(spec);
    std::string token;
    while (parts >> token) {
        tokens.push_back(token);
    }
    if (tokens.empty()) {
        tokens.push_back(spec);
    }
    return tokens;
}

std::optional<EditorAction> action_from_name(const std::string &name) {
    static const std::map<std::string, EditorAction> kActions = {
        {"move_left", EditorAction::MoveLeft},
        {"move_right", EditorAction::MoveRight},
        {"move_up", EditorAction::MoveUp},
        {"move_down", EditorAction::MoveDown},
        {"move_line_start", EditorAction::MoveLineStart},
        {"move_line_end", EditorAction::MoveLineEnd},
        {"find_forward", EditorAction::FindForward},
        {"find_backward", EditorAction::FindBackward},
        {"till_forward", EditorAction::TillForward},
        {"till_backward", EditorAction::TillBackward},
        {"enter_insert_mode", EditorAction::EnterInsertMode},
        {"append_after_cursor", EditorAction::AppendAfterCursor},
        {"enter_visual_mode", EditorAction::EnterVisualMode},
        {"enter_visual_line_mode", EditorAction::EnterVisualLineMode},
        {"append_line_end_insert", EditorAction::AppendLineEndInsert},
        {"insert_line_start_insert", EditorAction::InsertLineStartInsert},
        {"open_line_below", EditorAction::OpenLineBelow},
        {"open_line_above", EditorAction::OpenLineAbove},
        {"delete_char", EditorAction::DeleteChar},
        {"undo", EditorAction::Undo},
        {"redo", EditorAction::Redo},
        {"paste_after", EditorAction::PasteAfter},
        {"paste_before", EditorAction::PasteBefore},
        {"goto_top", EditorAction::GotoTop},
        {"goto_bottom", EditorAction::GotoBottom},
        {"enter_command_mode", EditorAction::EnterCommandMode},
        {"enter_search_mode", EditorAction::EnterSearchMode},
        {"delete_line", EditorAction::DeleteLine},
        {"half_page_down", EditorAction::HalfPageDown},
        {"half_page_up", EditorAction::HalfPageUp},
        {"page_up", EditorAction::PageUp},
        {"page_down", EditorAction::PageDown},
        {"suspend", EditorAction::Suspend},
        {"enter_normal_mode", EditorAction::EnterNormalMode},
        {"insert_newline", EditorAction::InsertNewline},
        {"backspace", EditorAction::Backspace},
        {"command_execute", EditorAction::CommandExecute},
        {"command_backspace", EditorAction::CommandBackspace},
        {"self_insert", EditorAction::SelfInsert},
        {"command_insert", EditorAction::CommandInsert},
        {"search_execute", EditorAction::SearchExecute},
        {"search_backspace", EditorAction::SearchBackspace},
        {"search_insert", EditorAction::SearchInsert},
        {"search_next", EditorAction::SearchNext},
        {"search_previous", EditorAction::SearchPrevious},
        {"next_diagnostic", EditorAction::NextDiagnostic},
        {"previous_diagnostic", EditorAction::PreviousDiagnostic},
        {"toggle_diagnostics_panel", EditorAction::ToggleDiagnosticsPanel},
        {"delete_selection", EditorAction::DeleteSelection},
        {"change_selection", EditorAction::ChangeSelection},
        {"yank_selection", EditorAction::YankSelection},
        {"replace_selection_with_yank", EditorAction::ReplaceSelectionWithYank},
        {"select_inner_word", EditorAction::SelectInnerWord},
        {"select_around_word", EditorAction::SelectAroundWord},
    };
    auto found = kActions.find(name);
    if (found == kActions.end()) {
        return std::nullopt;
    }
    return found->second;
}

KeyBindings parse_keybindings_source(const std::string &source, const std::string &origin) {
    JsonValue root = parse_json(source);
    if (root.type != JsonValue::Type::Object) {
        throw std::runtime_error("keybindings root must be an object");
    }

    KeyBindings keybindings;
    keybindings.source_path = origin;

    for (const auto &mode_entry : root.object_value) {
        if (mode_entry.second.type != JsonValue::Type::Object) {
            throw std::runtime_error("mode bindings must be objects");
        }
        for (const auto &binding_entry : mode_entry.second.object_value) {
            if (binding_entry.second.type != JsonValue::Type::String) {
                throw std::runtime_error("binding actions must be strings");
            }
            std::optional<EditorAction> action = action_from_name(binding_entry.second.string_value);
            if (!action) {
                throw std::runtime_error("unknown action in keybindings: " + binding_entry.second.string_value);
            }
            keybindings.bindings.push_back({mode_entry.first, split_key_sequence(binding_entry.first), *action});
        }
    }

    return keybindings;
}

bool sequence_matches_prefix(const std::vector<std::string> &sequence, const std::vector<std::string> &prefix) {
    if (prefix.size() > sequence.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (sequence[index] != prefix[index]) {
            return false;
        }
    }
    return true;
}

std::optional<EditorAction> wildcard_action_for_mode(const KeyBindings &keybindings, const std::string &mode) {
    for (const KeyBinding &binding : keybindings.bindings) {
        if (binding.mode == mode && binding.sequence.size() == 1 && binding.sequence[0] == "printable") {
            return binding.action;
        }
    }
    return std::nullopt;
}

KeyDispatch dispatch_single_attempt(
    const KeyBindings &keybindings,
    const std::string &mode,
    const std::vector<std::string> &tokens,
    bool printable) {
    bool has_prefix = false;
    for (const KeyBinding &binding : keybindings.bindings) {
        if (binding.mode != mode) {
            continue;
        }
        if (binding.sequence == tokens) {
            return {true, false, binding.action};
        }
        if (sequence_matches_prefix(binding.sequence, tokens)) {
            has_prefix = true;
        }
    }

    if (has_prefix) {
        return {true, true, std::nullopt};
    }

    if (tokens.size() == 1 && printable) {
        std::optional<EditorAction> wildcard = wildcard_action_for_mode(keybindings, mode);
        if (wildcard) {
            return {true, false, wildcard};
        }
    }

    return {false, false, std::nullopt};
}

}  // namespace

KeyBindings load_keybindings() {
    return load_keybindings(load_editor_config());
}

KeyBindings load_keybindings(const EditorConfig &config) {
    if (config.keybindings_path && std::filesystem::exists(*config.keybindings_path)) {
        return load_keybindings_from_path(*config.keybindings_path);
    }
    if (!config.source_path.empty() && config.keybindings_path) {
        throw std::runtime_error("configured keybindings file not found: " + config.keybindings_path->string());
    }
    return load_embedded_keybindings();
}

KeyBindings load_keybindings_from_path(const std::filesystem::path &path) {
    return parse_keybindings_source(read_text_file(path), path.string());
}

KeyBindings load_embedded_keybindings() {
    return parse_keybindings_source(kEmbeddedDefaultKeybindings, "<embedded>");
}

void remove_action_bindings(KeyBindings &keybindings, EditorAction action) {
    keybindings.bindings.erase(
        std::remove_if(
            keybindings.bindings.begin(),
            keybindings.bindings.end(),
            [action](const KeyBinding &binding) { return binding.action == action; }),
        keybindings.bindings.end());
}

KeyDispatch dispatch_key_sequence(
    const KeyBindings &keybindings,
    const std::string &mode,
    std::vector<std::string> &pending_tokens,
    const std::string &token,
    bool printable) {
    pending_tokens.push_back(token);
    KeyDispatch attempt = dispatch_single_attempt(keybindings, mode, pending_tokens, printable);
    if (attempt.matched) {
        if (!attempt.waiting_for_more) {
            pending_tokens.clear();
        }
        return attempt;
    }

    if (pending_tokens.size() > 1) {
        std::string retry = pending_tokens.back();
        pending_tokens.clear();
        pending_tokens.push_back(retry);
        KeyDispatch retry_attempt = dispatch_single_attempt(keybindings, mode, pending_tokens, printable);
        if (retry_attempt.matched) {
            if (!retry_attempt.waiting_for_more) {
                pending_tokens.clear();
            }
            return retry_attempt;
        }
    }

    pending_tokens.clear();
    return {};
}
