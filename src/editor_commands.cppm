module;

#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module editor_commands;

export import editor_core;

export enum class EditorCommandType {
    SetDiagnostics,
    ClearDiagnostics,
    SetAnnotations,
    ClearAnnotations,
    MoveCursor,
    OpenLocation,
    SetSelectionRange,
    ShowPopup,
    ClearPopup,
    SetStatusMessage,
};

export enum class PopupKind {
    Text,
    Menu,
    KeyHints,
};

export struct PopupMenuItem {
    std::string label;
    std::string detail;
    std::string insert_text;
    std::optional<Range> replace_range;
};

export using PopupMenuItems = std::vector<PopupMenuItem>;

export struct EditorCommand {
    EditorCommandType type = EditorCommandType::SetStatusMessage;
    std::optional<std::string> document_uri;
    Diagnostics diagnostics;
    InlineAnnotations annotations;
    std::optional<Position> position;
    std::optional<Range> selection_range;
    std::vector<Range> selection_ranges;
    std::string title;
    std::string message;
    PopupKind popup_kind = PopupKind::Text;
    PopupMenuItems popup_items;
};

export struct EditorCommandResult {
    bool applied = false;
    std::optional<std::string> status_message;
};

export EditorCommandResult apply_editor_command(EditorCore &core, const EditorCommand &command);
export bool popup_selection_accept_token(std::string_view token);
