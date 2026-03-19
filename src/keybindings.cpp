#include "keybindings.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr const char *kEmbeddedDefaultKeybindings = R"json(
{
  "normal": {
    "left": "move_left",
    "shift-left": "visual_move_left",
    "right": "move_right",
    "shift-right": "visual_move_right",
    "up": "move_up",
    "shift-up": "visual_move_up",
    "down": "move_down",
    "shift-down": "visual_move_down",
    "home": "move_line_start",
    "shift-home": "visual_move_line_start",
    "end": "move_line_end",
    "shift-end": "visual_move_line_end",
    "h": "move_left",
    "H": "visual_move_left",
    "j": "move_down",
    "J": "visual_move_down",
    "k": "move_up",
    "K": "visual_move_up",
    "l": "move_right",
    "L": "visual_move_right",
    "f": "find_forward",
    "F": "visual_find_forward",
    "t": "till_forward",
    "T": "visual_till_forward",
    "0": "move_line_start",
    ")": "visual_move_line_start",
    "$": "move_line_end",
    "^": "visual_move_line_end",
    "i": "enter_insert_mode",
    "a": "append_after_cursor",
    "v": "enter_visual_mode",
    "V": "enter_visual_line_mode",
    "A": "append_line_end_insert",
    "I": "insert_line_start_insert",
    "o": "open_line_below",
    "O": "open_line_above",
    "x": "delete_char",
    "r": "replace_char",
    ".": "repeat_last_command",
    "D": ["v", "$", "d"],
    "u": "undo",
    "ctrl-r": "redo",
    "p": "paste_after",
    "P": "paste_before",
    "%": "select_all",
    "g g": "goto_top",
    "g G": "visual_goto_top",
    "g e": "goto_bottom",
    "g E": "visual_goto_bottom",
    "/": "enter_search_mode",
    ":": "enter_command_mode",
    "n": "search_next",
    "N": "visual_search_next",
    "b": "search_previous",
    "B": "visual_search_previous",
    "?": "show_key_hints",
    "g d": "goto_definition",
    "g k": "show_hover",
    "alt-up": "select_enclosing_ast",
    "alt-down": "select_inner_ast",
    "ctrl-p": "show_completion",
    "g f": "find_backward",
    "g F": "visual_find_backward",
    "g t": "till_backward",
    "g T": "visual_till_backward",
    "g p": "goto_file_under_cursor",
    "g o": "jump_back",
    "g i": "jump_forward",
    "] d": "next_diagnostic",
    "[ d": "previous_diagnostic",
    "ctrl-g": "toggle_diagnostics_visibility",
    "g x": "toggle_diagnostics_panel",
    "d d": "delete_line",
    "ctrl-d": "half_page_down",
    "ctrl-u": "half_page_up",
    "ctrl-z": "suspend",
    ">": "indent",
    "<": "outdent",
    "tab": "next_buffer",
    "shift-tab": "previous_buffer",
    "ctrl-w s": "split_horizontal",
    "ctrl-w v": "split_vertical",
    "ctrl-w c": "close_window",
    "ctrl-w o": "close_other_windows",
    "ctrl-w h": "focus_window_left",
    "ctrl-w left": "focus_window_left",
    "ctrl-w l": "focus_window_right",
    "ctrl-w right": "focus_window_right",
    "ctrl-w k": "focus_window_up",
    "ctrl-w up": "focus_window_up",
    "ctrl-w j": "focus_window_down",
    "ctrl-w down": "focus_window_down",
    "pageup": "page_up",
    "pagedown": "page_down"
  },
  "visual": {
    "left": "move_left",
    "shift-left": "visual_move_left",
    "right": "move_right",
    "shift-right": "visual_move_right",
    "up": "move_up",
    "shift-up": "visual_move_up",
    "down": "move_down",
    "shift-down": "visual_move_down",
    "home": "move_line_start",
    "shift-home": "visual_move_line_start",
    "end": "move_line_end",
    "shift-end": "visual_move_line_end",
    "h": "move_left",
    "H": "visual_move_left",
    "j": "move_down",
    "J": "visual_move_down",
    "k": "move_up",
    "K": "visual_move_up",
    "l": "move_right",
    "L": "visual_move_right",
    "f": "find_forward",
    "F": "visual_find_forward",
    "t": "till_forward",
    "T": "visual_till_forward",
    "i w": "select_inner_word",
    "a w": "select_around_word",
    "0": "move_line_start",
    ")": "visual_move_line_start",
    "$": "move_line_end",
    "^": "visual_move_line_end",
    "g g": "goto_top",
    "g G": "visual_goto_top",
    "g e": "goto_bottom",
    "g E": "visual_goto_bottom",
    "n": "search_next",
    "N": "visual_search_next",
    "b": "search_previous",
    "B": "visual_search_previous",
    "?": "show_key_hints",
    "g d": "goto_definition",
    "g k": "show_hover",
    "alt-up": "select_enclosing_ast",
    "alt-down": "select_inner_ast",
    "ctrl-p": "show_completion",
    "g f": "find_backward",
    "g F": "visual_find_backward",
    "g t": "till_backward",
    "g T": "visual_till_backward",
    "g p": "goto_file_under_cursor",
    "g o": "jump_back",
    "g i": "jump_forward",
    "] d": "next_diagnostic",
    "[ d": "previous_diagnostic",
    "ctrl-g": "toggle_diagnostics_visibility",
    "|": "filter_selection",
    "S": "sed_selection",
    "c": "change_selection",
    "d": "delete_selection",
    "y": "yank_selection",
    "p": "replace_selection_with_yank",
    "P": "replace_selection_with_yank",
    "o": "move_to_selection_start",
    "O": "move_to_selection_end",
    "%": "select_all",
    "v": "enter_normal_mode",
    "V": "enter_visual_line_mode",
    "esc": "enter_normal_mode",
    "ctrl-d": "half_page_down",
    "ctrl-u": "half_page_up",
    "ctrl-z": "suspend",
    ">": "indent",
    "<": "outdent",
    "tab": "next_buffer",
    "shift-tab": "previous_buffer",
    "pageup": "page_up",
    "pagedown": "page_down"
  },
  "insert": {
    "esc": "enter_normal_mode",
    "enter": "insert_newline",
    "tab": "insert_soft_tab",
    "shift-tab": "insert_outdent",
    "ctrl-p": "show_completion",
    "backspace": "backspace",
    "left": "move_left",
    "right": "move_right",
    "up": "move_up",
    "down": "move_down",
    "ctrl-g": "toggle_diagnostics_visibility",
    "printable": "self_insert"
  },
  "command": {
    "esc": "enter_normal_mode",
    "enter": "command_execute",
    "tab": "show_command_completion",
    "backspace": "command_backspace",
    "left": "prompt_move_left",
    "right": "prompt_move_right",
    "home": "prompt_move_start",
    "end": "prompt_move_end",
    "up": "command_history_previous",
    "down": "command_history_next",
    "printable": "command_insert"
  },
  "search": {
    "esc": "enter_normal_mode",
    "enter": "search_execute",
    "backspace": "search_backspace",
    "left": "prompt_move_left",
    "right": "prompt_move_right",
    "home": "prompt_move_start",
    "end": "prompt_move_end",
    "up": "search_history_previous",
    "down": "search_history_next",
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

const std::map<std::string, EditorAction> &action_map() {
    static const std::map<std::string, EditorAction> kActions = {
        {"move_left", EditorAction::MoveLeft},
        {"visual_move_left", EditorAction::VisualMoveLeft},
        {"move_right", EditorAction::MoveRight},
        {"visual_move_right", EditorAction::VisualMoveRight},
        {"move_up", EditorAction::MoveUp},
        {"visual_move_up", EditorAction::VisualMoveUp},
        {"move_down", EditorAction::MoveDown},
        {"visual_move_down", EditorAction::VisualMoveDown},
        {"move_line_start", EditorAction::MoveLineStart},
        {"visual_move_line_start", EditorAction::VisualMoveLineStart},
        {"move_line_end", EditorAction::MoveLineEnd},
        {"visual_move_line_end", EditorAction::VisualMoveLineEnd},
        {"find_forward", EditorAction::FindForward},
        {"visual_find_forward", EditorAction::VisualFindForward},
        {"find_backward", EditorAction::FindBackward},
        {"visual_find_backward", EditorAction::VisualFindBackward},
        {"till_forward", EditorAction::TillForward},
        {"visual_till_forward", EditorAction::VisualTillForward},
        {"till_backward", EditorAction::TillBackward},
        {"visual_till_backward", EditorAction::VisualTillBackward},
        {"enter_insert_mode", EditorAction::EnterInsertMode},
        {"append_after_cursor", EditorAction::AppendAfterCursor},
        {"enter_visual_mode", EditorAction::EnterVisualMode},
        {"enter_visual_line_mode", EditorAction::EnterVisualLineMode},
        {"append_line_end_insert", EditorAction::AppendLineEndInsert},
        {"insert_line_start_insert", EditorAction::InsertLineStartInsert},
        {"open_line_below", EditorAction::OpenLineBelow},
        {"open_line_above", EditorAction::OpenLineAbove},
        {"delete_char", EditorAction::DeleteChar},
        {"replace_char", EditorAction::ReplaceChar},
        {"repeat_last_command", EditorAction::RepeatLastCommand},
        {"undo", EditorAction::Undo},
        {"redo", EditorAction::Redo},
        {"paste_after", EditorAction::PasteAfter},
        {"paste_before", EditorAction::PasteBefore},
        {"goto_top", EditorAction::GotoTop},
        {"visual_goto_top", EditorAction::VisualGotoTop},
        {"goto_bottom", EditorAction::GotoBottom},
        {"visual_goto_bottom", EditorAction::VisualGotoBottom},
        {"enter_command_mode", EditorAction::EnterCommandMode},
        {"enter_search_mode", EditorAction::EnterSearchMode},
        {"delete_line", EditorAction::DeleteLine},
        {"half_page_down", EditorAction::HalfPageDown},
        {"half_page_up", EditorAction::HalfPageUp},
        {"page_up", EditorAction::PageUp},
        {"page_down", EditorAction::PageDown},
        {"indent", EditorAction::Indent},
        {"outdent", EditorAction::Outdent},
        {"next_buffer", EditorAction::NextBuffer},
        {"previous_buffer", EditorAction::PreviousBuffer},
        {"split_horizontal", EditorAction::SplitHorizontal},
        {"split_vertical", EditorAction::SplitVertical},
        {"close_window", EditorAction::CloseWindow},
        {"close_other_windows", EditorAction::CloseOtherWindows},
        {"focus_window_left", EditorAction::FocusWindowLeft},
        {"focus_window_right", EditorAction::FocusWindowRight},
        {"focus_window_up", EditorAction::FocusWindowUp},
        {"focus_window_down", EditorAction::FocusWindowDown},
        {"suspend", EditorAction::Suspend},
        {"enter_normal_mode", EditorAction::EnterNormalMode},
        {"insert_newline", EditorAction::InsertNewline},
        {"insert_soft_tab", EditorAction::InsertSoftTab},
        {"insert_outdent", EditorAction::InsertOutdent},
        {"backspace", EditorAction::Backspace},
        {"command_execute", EditorAction::CommandExecute},
        {"command_backspace", EditorAction::CommandBackspace},
        {"prompt_move_left", EditorAction::PromptMoveLeft},
        {"prompt_move_right", EditorAction::PromptMoveRight},
        {"prompt_move_start", EditorAction::PromptMoveStart},
        {"prompt_move_end", EditorAction::PromptMoveEnd},
        {"command_history_previous", EditorAction::CommandHistoryPrevious},
        {"command_history_next", EditorAction::CommandHistoryNext},
        {"show_command_completion", EditorAction::ShowCommandCompletion},
        {"self_insert", EditorAction::SelfInsert},
        {"command_insert", EditorAction::CommandInsert},
        {"search_execute", EditorAction::SearchExecute},
        {"search_backspace", EditorAction::SearchBackspace},
        {"search_insert", EditorAction::SearchInsert},
        {"search_history_previous", EditorAction::SearchHistoryPrevious},
        {"search_history_next", EditorAction::SearchHistoryNext},
        {"search_next", EditorAction::SearchNext},
        {"visual_search_next", EditorAction::VisualSearchNext},
        {"search_previous", EditorAction::SearchPrevious},
        {"visual_search_previous", EditorAction::VisualSearchPrevious},
        {"goto_definition", EditorAction::GoToDefinition},
        {"show_hover", EditorAction::ShowHover},
        {"show_completion", EditorAction::ShowCompletion},
        {"select_enclosing_ast", EditorAction::SelectEnclosingAst},
        {"select_inner_ast", EditorAction::SelectInnerAst},
        {"goto_file_under_cursor", EditorAction::GoToFileUnderCursor},
        {"jump_back", EditorAction::JumpBack},
        {"jump_forward", EditorAction::JumpForward},
        {"next_diagnostic", EditorAction::NextDiagnostic},
        {"previous_diagnostic", EditorAction::PreviousDiagnostic},
        {"toggle_diagnostics_visibility", EditorAction::ToggleDiagnosticsVisibility},
        {"toggle_diagnostics_panel", EditorAction::ToggleDiagnosticsPanel},
        {"delete_selection", EditorAction::DeleteSelection},
        {"filter_selection", EditorAction::FilterSelection},
        {"sed_selection", EditorAction::SedSelection},
        {"change_selection", EditorAction::ChangeSelection},
        {"yank_selection", EditorAction::YankSelection},
        {"replace_selection_with_yank", EditorAction::ReplaceSelectionWithYank},
        {"select_all", EditorAction::SelectAll},
        {"move_to_selection_start", EditorAction::MoveToSelectionStart},
        {"move_to_selection_end", EditorAction::MoveToSelectionEnd},
        {"select_inner_word", EditorAction::SelectInnerWord},
        {"select_around_word", EditorAction::SelectAroundWord},
        {"show_key_hints", EditorAction::ShowKeyHints},
    };
    return kActions;
}

std::string action_name_impl(EditorAction action) {
    for (const auto &[name, value] : action_map()) {
        if (value == action) {
            return name;
        }
    }
    return "action";
}

std::string humanize_action_name(const std::string &name) {
    std::string result;
    result.reserve(name.size());
    for (char ch : name) {
        result.push_back(ch == '_' ? ' ' : ch);
    }
    return result;
}

std::string join_tokens(const std::vector<std::string> &tokens, std::size_t start_index) {
    std::string result;
    for (std::size_t index = start_index; index < tokens.size(); ++index) {
        if (!result.empty()) {
            result += ' ';
        }
        result += tokens[index];
    }
    return result;
}

std::string describe_binding(const KeyBinding &binding) {
    if (binding.action) {
        return humanize_action_name(action_name_impl(*binding.action));
    }
    return "exec " + join_tokens(binding.expansion, 0);
}

std::string summarize_descriptions(const std::vector<std::string> &parts) {
    std::string summary;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += parts[index];
    }
    return summary;
}

std::optional<EditorAction> action_from_name(const std::string &name) {
    auto found = action_map().find(name);
    if (found == action_map().end()) {
        return std::nullopt;
    }
    return found->second;
}

KeyBindings parse_keybindings_source(const std::string &source, const std::string &origin) {
    JsonValue root = parse_json(source);
    if (!root.is_object()) {
        throw std::runtime_error("keybindings root must be an object");
    }

    KeyBindings keybindings;
    keybindings.source_path = origin;

    for (const auto &mode_entry : root.items()) {
        if (!mode_entry.value().is_object()) {
            throw std::runtime_error("mode bindings must be objects");
        }
        for (const auto &binding_entry : mode_entry.value().items()) {
            if (binding_entry.value().is_string()) {
                std::optional<EditorAction> action = action_from_name(binding_entry.value().get<std::string>());
                if (!action) {
                    throw std::runtime_error("unknown action in keybindings: " + binding_entry.value().get<std::string>());
                }
                keybindings.bindings.push_back({mode_entry.key(), split_key_sequence(binding_entry.key()), *action, {}});
                continue;
            }
            if (binding_entry.value().is_array()) {
                std::vector<std::string> expansion;
                for (const JsonValue &value : binding_entry.value()) {
                    if (!value.is_string()) {
                        throw std::runtime_error("binding sequence entries must be strings");
                    }
                    expansion.push_back(value.get<std::string>());
                }
                if (expansion.empty()) {
                    throw std::runtime_error("binding sequence aliases must not be empty");
                }
                keybindings.bindings.push_back({mode_entry.key(), split_key_sequence(binding_entry.key()), std::nullopt, expansion});
                continue;
            }
            throw std::runtime_error("binding values must be strings or arrays");
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
            return {true, false, binding.action, binding.expansion};
        }
        if (sequence_matches_prefix(binding.sequence, tokens)) {
            has_prefix = true;
        }
    }

    if (has_prefix) {
        return {true, true, std::nullopt, {}};
    }

    if (tokens.size() == 1 && printable) {
        std::optional<EditorAction> wildcard = wildcard_action_for_mode(keybindings, mode);
        if (wildcard) {
            return {true, false, wildcard, {}};
        }
    }

    return {false, false, std::nullopt, {}};
}

}  // namespace

std::string action_name(EditorAction action) {
    return action_name_impl(action);
}

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

std::vector<KeyHint> key_hints_for_prefix(
    const KeyBindings &keybindings,
    const std::string &mode,
    const std::vector<std::string> &prefix) {
    struct HintAggregate {
        std::string token;
        std::vector<std::string> details;
    };

    std::vector<HintAggregate> aggregates;
    std::unordered_map<std::string, std::size_t> aggregate_index;

    for (const KeyBinding &binding : keybindings.bindings) {
        if (binding.mode != mode || binding.sequence.empty()) {
            continue;
        }
        if (binding.sequence.size() == 1 && binding.sequence[0] == "printable") {
            continue;
        }
        if (!sequence_matches_prefix(binding.sequence, prefix) || binding.sequence.size() <= prefix.size()) {
            continue;
        }

        const std::string &token = binding.sequence[prefix.size()];
        std::size_t index = 0;
        auto found = aggregate_index.find(token);
        if (found == aggregate_index.end()) {
            index = aggregates.size();
            aggregate_index.emplace(token, index);
            aggregates.push_back({token, {}});
        } else {
            index = found->second;
        }

        std::string detail;
        if (binding.sequence.size() == prefix.size() + 1) {
            detail = describe_binding(binding);
        } else {
            detail = join_tokens(binding.sequence, prefix.size() + 1) + " " + describe_binding(binding);
        }
        if (std::find(aggregates[index].details.begin(), aggregates[index].details.end(), detail) ==
            aggregates[index].details.end()) {
            aggregates[index].details.push_back(std::move(detail));
        }
    }

    std::vector<KeyHint> hints;
    hints.reserve(aggregates.size());
    for (const HintAggregate &aggregate : aggregates) {
        hints.push_back({aggregate.token, summarize_descriptions(aggregate.details)});
    }
    return hints;
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
