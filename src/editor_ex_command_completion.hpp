#pragma once

#include "editor_commands.hpp"

#include <optional>
#include <string>
#include <string_view>

struct EditFileCompletionResult {
    std::string initial_filter;
    PopupMenuItems items;
};

std::optional<EditFileCompletionResult> complete_edit_file_command(std::string_view command);
