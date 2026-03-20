#include "editor_command_palette.hpp"

#include <array>

namespace {

constexpr std::array<NamedEditorCommandInfo, 16> kNamedEditorCommands{{
    {"w", "write current buffer", "w", NamedEditorCommand::Write},
    {"q", "quit", "q", NamedEditorCommand::Quit},
    {"q!", "force quit", "q!", NamedEditorCommand::ForceQuit},
    {"wq", "write and quit", "wq", NamedEditorCommand::WriteQuit},
    {"x", "write and quit if modified", "x", NamedEditorCommand::WriteIfChangedQuit},
    {"e", "edit file in active window", "e ", NamedEditorCommand::Edit},
    {"buffers", "list open buffers", "buffers", NamedEditorCommand::Buffers},
    {"buffer", "switch to buffer", "buffer ", NamedEditorCommand::Buffer},
    {"bnext", "next buffer", "bnext", NamedEditorCommand::NextBuffer},
    {"bprev", "previous buffer", "bprev", NamedEditorCommand::PreviousBuffer},
    {"bd", "close current buffer", "bd", NamedEditorCommand::DeleteBuffer},
    {"bd!", "force close current buffer", "bd!", NamedEditorCommand::ForceDeleteBuffer},
    {"reload-config", "reload medit configuration", "reload-config", NamedEditorCommand::ReloadConfig},
    {"diagnostics", "show diagnostics summary", "diagnostics", NamedEditorCommand::Diagnostics},
    {"lsp-status", "show language server status", "lsp-status", NamedEditorCommand::LspStatus},
    {"tree-sitter-status", "show tree-sitter syntax status", "tree-sitter-status", NamedEditorCommand::TreeSitterStatus},
}};

}  // namespace

std::span<const NamedEditorCommandInfo> named_editor_commands() {
    return kNamedEditorCommands;
}

NamedEditorCommandResolution resolve_named_editor_command(std::string_view verb) {
    for (const NamedEditorCommandInfo &command : kNamedEditorCommands) {
        if (command.name == verb) {
            return {command.command, false};
        }
    }

    std::optional<NamedEditorCommand> match;
    for (const NamedEditorCommandInfo &command : kNamedEditorCommands) {
        if (!command.name.starts_with(verb)) {
            continue;
        }
        if (match.has_value()) {
            return {std::nullopt, true};
        }
        match = command.command;
    }
    return {match, false};
}
