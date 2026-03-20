#pragma once

import editor_core;

#include "config.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <expected>

enum class SyntaxEngine {
    None,
    TreeSitter,
};

struct SyntaxSelection {
    SyntaxEngine engine = SyntaxEngine::None;
    std::string language_name;
};

bool operator==(const SyntaxSelection &left, const SyntaxSelection &right);
bool operator!=(const SyntaxSelection &left, const SyntaxSelection &right);

SyntaxSelection resolve_syntax_selection(const EditorConfig &config, const std::optional<std::string> &file_path);
std::expected<std::vector<HighlightSpans>, std::string> highlight_document_syntax(
    const Lines &lines,
    const EditorConfig &config,
    const SyntaxSelection &selection);
std::string tree_sitter_status_summary(const EditorConfig &config, const std::optional<std::string> &file_path);
std::string tree_sitter_health_summary(const EditorConfig &config);
void invalidate_syntax_runtime_cache();
