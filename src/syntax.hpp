#pragma once

#include "config.hpp"
#include "editor_core.hpp"

#include <optional>
#include <string>
#include <vector>

enum class SyntaxMode {
    None,
    Cpp,
};

SyntaxMode syntax_mode_from_name(const std::string &name);
SyntaxMode detect_syntax_mode(const std::optional<std::string> &file_path);
SyntaxMode resolve_syntax_mode(const EditorConfig &config, const std::optional<std::string> &file_path);
std::vector<std::vector<HighlightSpan>> highlight_document_syntax(
    const std::vector<std::u32string> &lines,
    SyntaxMode mode);
