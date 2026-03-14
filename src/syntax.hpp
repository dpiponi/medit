#pragma once

#include "config.hpp"
#include "editor_core.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>

enum class SyntaxEngine {
    None,
    LegacyCpp,
    TreeSitter,
};

struct SyntaxSelection {
    SyntaxEngine engine = SyntaxEngine::None;
    std::string language_name;
};

bool operator==(const SyntaxSelection &left, const SyntaxSelection &right);
bool operator!=(const SyntaxSelection &left, const SyntaxSelection &right);

SyntaxSelection resolve_syntax_selection(const EditorConfig &config, const std::optional<std::string> &file_path);
std::expected<std::vector<std::vector<HighlightSpan>>, std::string> highlight_document_syntax(
    const std::vector<std::u32string> &lines,
    const EditorConfig &config,
    const SyntaxSelection &selection);
void invalidate_syntax_runtime_cache();
