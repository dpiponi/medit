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
    SetStatusMessage,
};

struct EditorCommand {
    EditorCommandType type = EditorCommandType::SetStatusMessage;
    std::optional<std::string> document_uri;
    std::vector<Diagnostic> diagnostics;
    std::vector<InlineAnnotation> annotations;
    std::optional<Position> position;
    std::string message;
};

struct EditorCommandResult {
    bool applied = false;
    std::optional<std::string> status_message;
};

EditorCommandResult apply_editor_command(EditorCore &core, const EditorCommand &command);
