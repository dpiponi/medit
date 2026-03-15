#pragma once

#include "config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class EditorAction {
    MoveLeft,
    VisualMoveLeft,
    MoveRight,
    VisualMoveRight,
    MoveUp,
    VisualMoveUp,
    MoveDown,
    VisualMoveDown,
    MoveLineStart,
    VisualMoveLineStart,
    MoveLineEnd,
    VisualMoveLineEnd,
    FindForward,
    VisualFindForward,
    FindBackward,
    VisualFindBackward,
    TillForward,
    VisualTillForward,
    TillBackward,
    VisualTillBackward,
    EnterInsertMode,
    AppendAfterCursor,
    EnterVisualMode,
    EnterVisualLineMode,
    AppendLineEndInsert,
    InsertLineStartInsert,
    OpenLineBelow,
    OpenLineAbove,
    DeleteChar,
    ReplaceChar,
    RepeatLastCommand,
    Undo,
    Redo,
    PasteAfter,
    PasteBefore,
    GotoTop,
    VisualGotoTop,
    GotoBottom,
    VisualGotoBottom,
    EnterCommandMode,
    EnterSearchMode,
    DeleteLine,
    HalfPageDown,
    HalfPageUp,
    PageUp,
    PageDown,
    Indent,
    Outdent,
    NextBuffer,
    PreviousBuffer,
    SplitHorizontal,
    SplitVertical,
    CloseWindow,
    CloseOtherWindows,
    FocusWindowLeft,
    FocusWindowRight,
    FocusWindowUp,
    FocusWindowDown,
    Suspend,
    EnterNormalMode,
    InsertNewline,
    InsertSoftTab,
    InsertOutdent,
    Backspace,
    CommandExecute,
    CommandBackspace,
    CommandHistoryPrevious,
    CommandHistoryNext,
    ShowCommandCompletion,
    SelfInsert,
    CommandInsert,
    SearchExecute,
    SearchBackspace,
    SearchInsert,
    SearchHistoryPrevious,
    SearchHistoryNext,
    SearchNext,
    VisualSearchNext,
    SearchPrevious,
    VisualSearchPrevious,
    GoToDefinition,
    ShowHover,
    ShowCompletion,
    SelectEnclosingAst,
    SelectInnerAst,
    GoToFileUnderCursor,
    JumpBack,
    JumpForward,
    NextDiagnostic,
    PreviousDiagnostic,
    ToggleDiagnosticsVisibility,
    ToggleDiagnosticsPanel,
    DeleteSelection,
    FilterSelection,
    SedSelection,
    ChangeSelection,
    YankSelection,
    ReplaceSelectionWithYank,
    SelectAll,
    MoveToSelectionStart,
    MoveToSelectionEnd,
    SelectInnerWord,
    SelectAroundWord,
};

struct KeyBinding {
    std::string mode;
    std::vector<std::string> sequence;
    std::optional<EditorAction> action;
    std::vector<std::string> expansion;
};

struct KeyBindings {
    std::vector<KeyBinding> bindings;
    std::string source_path;
};

struct KeyDispatch {
    bool matched = false;
    bool waiting_for_more = false;
    std::optional<EditorAction> action;
    std::vector<std::string> expansion;
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
