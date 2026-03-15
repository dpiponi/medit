#pragma once

#include "editor_core.hpp"

#include <optional>
#include <string>
#include <vector>

enum class EditorCommandType {
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

enum class PopupKind {
    Text,
    Menu,
    KeyHints,
};

struct PopupMenuItem {
    std::string label;
    std::string detail;
    std::string insert_text;
    std::optional<Range> replace_range;
};

struct EditorCommand {
    EditorCommandType type = EditorCommandType::SetStatusMessage;
    std::optional<std::string> document_uri;
    std::vector<Diagnostic> diagnostics;
    std::vector<InlineAnnotation> annotations;
    std::optional<Position> position;
    std::optional<Range> selection_range;
    std::vector<Range> selection_ranges;
    std::string title;
    std::string message;
    PopupKind popup_kind = PopupKind::Text;
    std::vector<PopupMenuItem> popup_items;
};

struct EditorCommandResult {
    bool applied = false;
    std::optional<std::string> status_message;
};

EditorCommandResult apply_editor_command(EditorCore &core, const EditorCommand &command);
