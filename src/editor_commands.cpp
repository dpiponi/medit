module;

#include <string_view>

module editor_commands;

EditorCommandResult apply_editor_command(EditorCore &core, const EditorCommand &command) {
    EditorCommandResult result;

    switch (command.type) {
        case EditorCommandType::SetDiagnostics:
            if (command.document_uri) {
                core.set_document_diagnostics(*command.document_uri, command.diagnostics);
            } else {
                core.set_diagnostics(command.diagnostics);
            }
            result.applied = true;
            return result;
        case EditorCommandType::ClearDiagnostics:
            if (command.document_uri) {
                core.clear_document_diagnostics(*command.document_uri);
            } else {
                core.clear_diagnostics();
            }
            result.applied = true;
            return result;
        case EditorCommandType::SetAnnotations:
            if (command.document_uri) {
                core.set_document_annotations(*command.document_uri, command.annotations);
            } else {
                core.set_annotations(command.annotations);
            }
            result.applied = true;
            return result;
        case EditorCommandType::ClearAnnotations:
            if (command.document_uri) {
                core.clear_document_annotations(*command.document_uri);
            } else {
                core.clear_annotations();
            }
            result.applied = true;
            return result;
        case EditorCommandType::MoveCursor:
            if (command.position) {
                core.set_cursor(*command.position);
                result.applied = true;
            }
            return result;
        case EditorCommandType::SetSelectionRange:
            if (command.selection_range) {
                result.applied = core.set_selection_range(*command.selection_range, SelectionMode::Character);
            }
            return result;
        case EditorCommandType::OpenLocation:
        case EditorCommandType::ShowPopup:
        case EditorCommandType::ClearPopup:
            return result;
        case EditorCommandType::SetStatusMessage:
            result.applied = true;
            result.status_message = command.message;
            return result;
    }

    return result;
}

bool popup_selection_accept_token(std::string_view token) {
    return token == "tab" || token == "enter";
}
