#pragma once

#include "config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class EditorAction {
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    MoveLineStart,
    MoveLineEnd,
    FindForward,
    FindBackward,
    TillForward,
    TillBackward,
    EnterInsertMode,
    AppendAfterCursor,
    EnterVisualMode,
    EnterVisualLineMode,
    AppendLineEndInsert,
    InsertLineStartInsert,
    OpenLineBelow,
    OpenLineAbove,
    DeleteChar,
    Undo,
    Redo,
    PasteAfter,
    PasteBefore,
    GotoTop,
    GotoBottom,
    EnterCommandMode,
    EnterSearchMode,
    DeleteLine,
    HalfPageDown,
    HalfPageUp,
    PageUp,
    PageDown,
    Suspend,
    EnterNormalMode,
    InsertNewline,
    Backspace,
    CommandExecute,
    CommandBackspace,
    SelfInsert,
    CommandInsert,
    SearchExecute,
    SearchBackspace,
    SearchInsert,
    SearchNext,
    SearchPrevious,
    DeleteSelection,
    ChangeSelection,
    YankSelection,
    ReplaceSelectionWithYank,
    SelectInnerWord,
    SelectAroundWord,
};

struct KeyBinding {
    std::string mode;
    std::vector<std::string> sequence;
    EditorAction action;
};

struct KeyBindings {
    std::vector<KeyBinding> bindings;
    std::string source_path;
};

struct KeyDispatch {
    bool matched = false;
    bool waiting_for_more = false;
    std::optional<EditorAction> action;
};

KeyBindings load_keybindings();
KeyBindings load_keybindings(const EditorConfig &config);
KeyBindings load_keybindings_from_path(const std::filesystem::path &path);
KeyBindings load_embedded_keybindings();
void remove_action_bindings(KeyBindings &keybindings, EditorAction action);
KeyDispatch dispatch_key_sequence(
    const KeyBindings &keybindings,
    const std::string &mode,
    std::vector<std::string> &pending_tokens,
    const std::string &token,
    bool printable);
