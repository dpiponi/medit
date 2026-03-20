#pragma once

#include <optional>
#include <span>
#include <string_view>

enum class NamedEditorCommand {
    Write,
    Quit,
    ForceQuit,
    WriteQuit,
    WriteIfChangedQuit,
    Edit,
    Buffers,
    Buffer,
    NextBuffer,
    PreviousBuffer,
    DeleteBuffer,
    ForceDeleteBuffer,
    ReloadConfig,
    Diagnostics,
    LspStatus,
    TreeSitterStatus,
};

struct NamedEditorCommandInfo {
    std::string_view name;
    std::string_view detail;
    std::string_view completion_text;
    NamedEditorCommand command;
};

struct NamedEditorCommandResolution {
    std::optional<NamedEditorCommand> command;
    bool ambiguous = false;
};

std::span<const NamedEditorCommandInfo> named_editor_commands();
NamedEditorCommandResolution resolve_named_editor_command(std::string_view verb);
