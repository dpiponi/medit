#include "syntax.hpp"

#include "logger.hpp"
#include "position_utils.hpp"
#include "string_utils.hpp"
#include "text_encoding_utils.hpp"

#include <expected>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace {

struct TSLanguage;
struct TSParser;
struct TSTree;
struct TSQuery;
struct TSQueryCursor;

struct TSPoint {
    uint32_t row;
    uint32_t column;
};

struct TSNode {
    uint32_t context[4];
    const void *id;
    const void *tree;
};

struct TSQueryCapture {
    TSNode node;
    uint32_t index;
};

struct TSQueryMatch {
    uint32_t id;
    uint16_t pattern_index;
    uint16_t capture_count;
    const TSQueryCapture *captures;
};

enum class TSQueryError : uint32_t {
    None = 0,
    Syntax = 1,
    NodeType = 2,
    Field = 3,
    Capture = 4,
    Structure = 5,
    Language = 6,
};

struct TreeSitterApi {
    void *handle = nullptr;
    std::string error;
    TSParser *(*parser_new)() = nullptr;
    void (*parser_delete)(TSParser *) = nullptr;
    bool (*parser_set_language)(TSParser *, const TSLanguage *) = nullptr;
    TSTree *(*parser_parse_string)(TSParser *, const TSTree *, const char *, uint32_t) = nullptr;
    void (*tree_delete)(TSTree *) = nullptr;
    TSNode (*tree_root_node)(const TSTree *) = nullptr;
    bool (*node_is_null)(TSNode) = nullptr;
    TSNode (*node_parent)(TSNode) = nullptr;
    TSNode (*node_named_descendant_for_point_range)(TSNode, TSPoint, TSPoint) = nullptr;
    TSPoint (*node_start_point)(TSNode) = nullptr;
    TSPoint (*node_end_point)(TSNode) = nullptr;
    TSQuery *(*query_new)(const TSLanguage *, const char *, uint32_t, uint32_t *, TSQueryError *) = nullptr;
    void (*query_delete)(TSQuery *) = nullptr;
    uint32_t (*query_capture_count)(const TSQuery *) = nullptr;
    const char *(*query_capture_name_for_id)(const TSQuery *, uint32_t, uint32_t *) = nullptr;
    TSQueryCursor *(*query_cursor_new)() = nullptr;
    void (*query_cursor_delete)(TSQueryCursor *) = nullptr;
    void (*query_cursor_exec)(TSQueryCursor *, const TSQuery *, TSNode) = nullptr;
    bool (*query_cursor_next_capture)(TSQueryCursor *, TSQueryMatch *, uint32_t *) = nullptr;

    bool loaded() const {
        return handle != nullptr;
    }
};

struct LoadedTreeSitterLanguage {
    void *grammar_handle = nullptr;
    const TSLanguage *language = nullptr;
    TSParser *parser = nullptr;
    TSQuery *query = nullptr;
    std::vector<std::string> capture_names;
};

void push_span(
    std::vector<HighlightSpans> &spans_by_line,
    std::size_t row,
    std::size_t start,
    std::size_t end,
    StyleRole role,
    int priority = 5) {
    if (row >= spans_by_line.size() || start >= end) {
        return;
    }
    spans_by_line[row].push_back({{{row, start}, {row, end}}, role, priority});
}

std::optional<const SyntaxLanguageConfig *> syntax_language_by_name(const EditorConfig &config, const std::string &name) {
    for (const SyntaxLanguageConfig &language : config.syntax_languages) {
        if (language.name == name) {
            return &language;
        }
    }
    return std::nullopt;
}

std::optional<const SyntaxLanguageConfig *> syntax_language_for_file(
    const EditorConfig &config,
    const std::optional<std::string> &file_path) {
    if (!file_path || file_path->empty()) {
        return std::nullopt;
    }
    for (const SyntaxLanguageConfig &language : config.syntax_languages) {
        for (const std::string &pattern : language.patterns) {
            if (file_path_matches_glob(*file_path, pattern)) {
                return &language;
            }
        }
    }
    return std::nullopt;
}

TreeSitterApi load_tree_sitter_api() {
    TreeSitterApi api;
#if defined(__unix__) || defined(__APPLE__)
    std::vector<std::string> candidates;
    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        std::filesystem::path home_path(home);
        candidates.push_back((home_path / ".config/medit/libtree-sitter.so").string());
        candidates.push_back((home_path / ".config/medit/libtree-sitter.dylib").string());
    }
    candidates.push_back("libtree-sitter.so.0");
    candidates.push_back("libtree-sitter.so");
    candidates.push_back("libtree-sitter.dylib");
    candidates.push_back("/usr/local/lib/libtree-sitter.dylib");
    candidates.push_back("/opt/homebrew/lib/libtree-sitter.dylib");

    for (const std::string &candidate : candidates) {
        log_debug(std::string("syntax runtime probe path=") + candidate);
        api.handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (api.handle) {
            log_debug(std::string("syntax runtime loaded path=") + candidate);
            break;
        }
    }
    if (!api.handle) {
        api.error = "tree-sitter runtime library not found";
        log_debug("syntax runtime load failed: " + api.error);
        return api;
    }

    auto load = [&](auto &slot, const char *symbol) -> bool {
        using SlotType = std::remove_reference_t<decltype(slot)>;
        slot = reinterpret_cast<SlotType>(dlsym(api.handle, symbol));
        return slot != nullptr;
    };

    if (!load(api.parser_new, "ts_parser_new") ||
        !load(api.parser_delete, "ts_parser_delete") ||
        !load(api.parser_set_language, "ts_parser_set_language") ||
        !load(api.parser_parse_string, "ts_parser_parse_string") ||
        !load(api.tree_delete, "ts_tree_delete") ||
        !load(api.tree_root_node, "ts_tree_root_node") ||
        !load(api.node_is_null, "ts_node_is_null") ||
        !load(api.node_parent, "ts_node_parent") ||
        !load(api.node_named_descendant_for_point_range, "ts_node_named_descendant_for_point_range") ||
        !load(api.node_start_point, "ts_node_start_point") ||
        !load(api.node_end_point, "ts_node_end_point") ||
        !load(api.query_new, "ts_query_new") ||
        !load(api.query_delete, "ts_query_delete") ||
        !load(api.query_capture_count, "ts_query_capture_count") ||
        !load(api.query_capture_name_for_id, "ts_query_capture_name_for_id") ||
        !load(api.query_cursor_new, "ts_query_cursor_new") ||
        !load(api.query_cursor_delete, "ts_query_cursor_delete") ||
        !load(api.query_cursor_exec, "ts_query_cursor_exec") ||
        !load(api.query_cursor_next_capture, "ts_query_cursor_next_capture")) {
        api.error = "tree-sitter runtime is missing required symbols";
        log_debug("syntax runtime load failed: " + api.error);
        dlclose(api.handle);
        api.handle = nullptr;
    }
#else
    api.error = "tree-sitter runtime loading unsupported on this platform";
    log_debug("syntax runtime load failed: " + api.error);
#endif
    return api;
}

TreeSitterApi &tree_sitter_api() {
    static TreeSitterApi api = load_tree_sitter_api();
    return api;
}

std::unordered_map<std::string, LoadedTreeSitterLanguage> &language_cache() {
    static std::unordered_map<std::string, LoadedTreeSitterLanguage> cache;
    return cache;
}

std::string cache_key_for_language(const SyntaxLanguageConfig &language) {
    return language.name + "|" + language.grammar_path.string() + "|" + language.symbol_name + "|" + language.highlights_path.string();
}

std::string read_text_file_or_throw(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open file: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::expected<LoadedTreeSitterLanguage *, std::string> load_tree_sitter_language(const SyntaxLanguageConfig &language) {
    TreeSitterApi &api = tree_sitter_api();
    if (!api.loaded()) {
        log_debug("syntax language load failed name=" + language.name + " error=" + api.error);
        return std::unexpected(api.error);
    }

    std::string key = cache_key_for_language(language);
    auto cached = language_cache().find(key);
    if (cached != language_cache().end()) {
        return &cached->second;
    }

#if defined(__unix__) || defined(__APPLE__)
    void *grammar_handle = dlopen(language.grammar_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!grammar_handle) {
        std::string error = "could not load grammar library: " + language.grammar_path.string();
        log_debug("syntax language load failed name=" + language.name + " error=" + error);
        return std::unexpected(error);
    }

    using LanguageFactory = const TSLanguage *(*)();
    LanguageFactory factory = reinterpret_cast<LanguageFactory>(dlsym(grammar_handle, language.symbol_name.c_str()));
    if (!factory) {
        dlclose(grammar_handle);
        std::string error = "could not load grammar symbol: " + language.symbol_name;
        log_debug("syntax language load failed name=" + language.name + " error=" + error);
        return std::unexpected(error);
    }

    TSParser *parser = api.parser_new();
    if (!parser) {
        dlclose(grammar_handle);
        std::string error = "could not create tree-sitter parser";
        log_debug("syntax language load failed name=" + language.name + " error=" + error);
        return std::unexpected(error);
    }

    const TSLanguage *ts_language = factory();
    if (!ts_language || !api.parser_set_language(parser, ts_language)) {
        api.parser_delete(parser);
        dlclose(grammar_handle);
        std::string error = "could not attach grammar language";
        log_debug("syntax language load failed name=" + language.name + " error=" + error);
        return std::unexpected(error);
    }

    std::string highlights_source;
    try {
        highlights_source = read_text_file_or_throw(language.highlights_path);
    } catch (const std::exception &error) {
        api.parser_delete(parser);
        dlclose(grammar_handle);
        std::string message = error.what();
        log_debug("syntax language load failed name=" + language.name + " error=" + message);
        return std::unexpected(message);
    }

    uint32_t error_offset = 0;
    TSQueryError query_error = TSQueryError::None;
    TSQuery *query = api.query_new(
        ts_language,
        highlights_source.c_str(),
        static_cast<uint32_t>(highlights_source.size()),
        &error_offset,
        &query_error);
    if (!query) {
        api.parser_delete(parser);
        dlclose(grammar_handle);
        std::string error = "could not compile highlights query: " + language.highlights_path.string();
        log_debug("syntax language load failed name=" + language.name + " error=" + error);
        return std::unexpected(error);
    }

    LoadedTreeSitterLanguage loaded;
    loaded.grammar_handle = grammar_handle;
    loaded.language = ts_language;
    loaded.parser = parser;
    loaded.query = query;
    uint32_t capture_count = api.query_capture_count(query);
    loaded.capture_names.reserve(capture_count);
    for (uint32_t index = 0; index < capture_count; ++index) {
        uint32_t length = 0;
        const char *name = api.query_capture_name_for_id(query, index, &length);
        loaded.capture_names.emplace_back(name == nullptr ? "" : std::string(name, length));
    }

    auto inserted = language_cache().emplace(key, std::move(loaded));
    log_debug("syntax language loaded name=" + language.name);
    return &inserted.first->second;
#else
    (void)language;
    std::string error = "tree-sitter runtime loading unsupported on this platform";
    log_debug("syntax language load failed error=" + error);
    return std::unexpected(error);
#endif
}

std::size_t codepoint_column_for_utf8_byte(const std::u32string &line, std::size_t byte_column) {
    std::size_t bytes = 0;
    for (std::size_t column = 0; column < line.size(); ++column) {
        std::size_t width = u32_to_utf8(std::u32string(1, line[column])).size();
        if (byte_column < bytes + width) {
            return column;
        }
        bytes += width;
        if (byte_column == bytes) {
            return column + 1;
        }
    }
    return line.size();
}

std::size_t utf8_byte_column_for_codepoint(const std::u32string &line, std::size_t codepoint_column) {
    std::size_t bytes = 0;
    std::size_t clamped = std::min(codepoint_column, line.size());
    for (std::size_t index = 0; index < clamped; ++index) {
        bytes += u32_to_utf8(std::u32string(1, line[index])).size();
    }
    return bytes;
}

std::string buffer_text_utf8(const Lines &lines) {
    std::u32string text;
    for (std::size_t row = 0; row < lines.size(); ++row) {
        text += lines[row];
        if (row + 1 < lines.size()) {
            text.push_back(U'\n');
        }
    }
    return u32_to_utf8(text);
}

TSPoint point_for_position(const Lines &lines, Position position) {
    if (lines.empty()) {
        return {0, 0};
    }
    std::size_t row = std::min(position.row, lines.size() - 1);
    std::size_t column = std::min(position.column, lines[row].size());
    return {static_cast<uint32_t>(row), static_cast<uint32_t>(utf8_byte_column_for_codepoint(lines[row], column))};
}

std::optional<Range> range_from_ts_node(const Lines &lines, const TreeSitterApi &api, TSNode node) {
    if (lines.empty() || api.node_is_null(node)) {
        return std::nullopt;
    }

    TSPoint start = api.node_start_point(node);
    TSPoint end = api.node_end_point(node);
    if (start.row >= lines.size()) {
        return std::nullopt;
    }

    std::size_t end_row = std::min<std::size_t>(end.row, lines.size() - 1);
    Range range{
        {start.row, codepoint_column_for_utf8_byte(lines[start.row], start.column)},
        {end_row, codepoint_column_for_utf8_byte(lines[end_row], end.column)},
    };
    range = normalized_range(range);
    if (range.start == range.end) {
        return std::nullopt;
    }
    return range;
}

StyleRole capture_name_to_style_role(std::string_view capture_name) {
    if (capture_name.contains("comment")) {
        return StyleRole::SyntaxComment;
    }
    if (capture_name.contains("string") ||
        capture_name.contains("character") ||
        capture_name.contains("escape") ||
        capture_name.contains("embedded")) {
        return StyleRole::SyntaxString;
    }
    if (capture_name.contains("function.builtin") ||
        capture_name.contains("constructor") ||
        capture_name.contains("variable.builtin") ||
        capture_name.contains("constant.builtin")) {
        return StyleRole::SyntaxBuiltin;
    }
    if (capture_name.contains("function") ||
        capture_name.contains("method")) {
        return StyleRole::SyntaxFunction;
    }
    if (capture_name.contains("property") ||
        capture_name.contains("field") ||
        capture_name.contains("attribute")) {
        return StyleRole::SyntaxProperty;
    }
    if (capture_name.contains("number") ||
        capture_name.contains("float") ||
        capture_name.contains("integer")) {
        return StyleRole::SyntaxNumber;
    }
    if (capture_name.contains("operator") ||
        capture_name.contains("punctuation")) {
        return StyleRole::SyntaxOperator;
    }
    if (capture_name.contains("type") ||
        capture_name.contains("module") ||
        capture_name.contains("namespace")) {
        return StyleRole::SyntaxType;
    }
    if (capture_name.contains("constant") ||
        capture_name.contains("boolean")) {
        return StyleRole::SyntaxConstant;
    }
    if (capture_name.contains("keyword") ||
        capture_name.contains("conditional") ||
        capture_name.contains("repeat") ||
        capture_name.contains("exception") ||
        capture_name.contains("include") ||
        capture_name.contains("define") ||
        capture_name.contains("preproc")) {
        return StyleRole::SyntaxKeyword;
    }
    return StyleRole::DefaultText;
}

std::expected<std::vector<HighlightSpans>, std::string> highlight_tree_sitter_document(
    const Lines &lines,
    const SyntaxLanguageConfig &language) {
    std::vector<HighlightSpans> spans_by_line(lines.size());
    std::expected<LoadedTreeSitterLanguage *, std::string> loaded = load_tree_sitter_language(language);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }

    TreeSitterApi &api = tree_sitter_api();
    std::string source;
    for (std::size_t row = 0; row < lines.size(); ++row) {
        source += u32_to_utf8(lines[row]);
        if (row + 1 < lines.size()) {
            source.push_back('\n');
        }
    }

    TSTree *tree = api.parser_parse_string(
        (*loaded)->parser,
        nullptr,
        source.c_str(),
        static_cast<uint32_t>(source.size()));
    if (!tree) {
        std::string error = "tree-sitter parse failed for " + language.name;
        log_debug("syntax highlight failed name=" + language.name + " error=" + error);
        return std::unexpected(error);
    }

    TSQueryCursor *cursor = api.query_cursor_new();
    if (!cursor) {
        api.tree_delete(tree);
        std::string error = "could not create tree-sitter query cursor";
        log_debug("syntax highlight failed name=" + language.name + " error=" + error);
        return std::unexpected(error);
    }

    api.query_cursor_exec(cursor, (*loaded)->query, api.tree_root_node(tree));
    TSQueryMatch match{};
    uint32_t capture_index = 0;
    while (api.query_cursor_next_capture(cursor, &match, &capture_index)) {
        if (capture_index >= match.capture_count) {
            continue;
        }
        const TSQueryCapture &capture = match.captures[capture_index];
        if (capture.index >= (*loaded)->capture_names.size()) {
            continue;
        }

        StyleRole role = capture_name_to_style_role((*loaded)->capture_names[capture.index]);
        if (role == StyleRole::DefaultText) {
            continue;
        }

        TSPoint start = api.node_start_point(capture.node);
        TSPoint end = api.node_end_point(capture.node);
        if (start.row >= lines.size()) {
            continue;
        }
        std::size_t end_row = std::min<std::size_t>(end.row, lines.empty() ? 0 : lines.size() - 1);
        for (std::size_t row = start.row; row <= end_row && row < lines.size(); ++row) {
            std::size_t start_column = row == start.row
                ? codepoint_column_for_utf8_byte(lines[row], start.column)
                : 0;
            std::size_t end_column = row == end.row
                ? codepoint_column_for_utf8_byte(lines[row], end.column)
                : lines[row].size();
            if (row != end.row && end.row >= lines.size()) {
                end_column = lines[row].size();
            }
            if (row < end.row) {
                end_column = lines[row].size();
            }
            push_span(spans_by_line, row, start_column, end_column, role, 5);
        }
    }

    api.query_cursor_delete(cursor);
    api.tree_delete(tree);
    return spans_by_line;
}

std::expected<std::vector<Range>, std::string> selection_ranges_tree_sitter_document(
    const Lines &lines,
    const SyntaxLanguageConfig &language,
    Position position) {
    std::expected<LoadedTreeSitterLanguage *, std::string> loaded = load_tree_sitter_language(language);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }

    TreeSitterApi &api = tree_sitter_api();
    std::string source = buffer_text_utf8(lines);
    TSTree *tree = api.parser_parse_string((*loaded)->parser, nullptr, source.c_str(), static_cast<uint32_t>(source.size()));
    if (!tree) {
        return std::unexpected("tree-sitter parse failed for " + language.name);
    }

    TSNode root = api.tree_root_node(tree);
    TSNode node = api.node_named_descendant_for_point_range(root, point_for_position(lines, position), point_for_position(lines, position));
    std::vector<Range> ranges;
    while (!api.node_is_null(node)) {
        if (std::optional<Range> range = range_from_ts_node(lines, api, node)) {
            if (ranges.empty() ||
                !(ranges.back().start == range->start && ranges.back().end == range->end)) {
                ranges.push_back(*range);
            }
        }
        node = api.node_parent(node);
    }
    api.tree_delete(tree);

    if (ranges.empty()) {
        return std::unexpected("No enclosing AST range");
    }
    return ranges;
}

std::string syntax_engine_name(SyntaxEngine engine) {
    switch (engine) {
        case SyntaxEngine::None:
            return "none";
        case SyntaxEngine::TreeSitter:
            return "tree-sitter";
    }
    return "unknown";
}

}  // namespace

bool operator==(const SyntaxSelection &left, const SyntaxSelection &right) {
    return left.engine == right.engine && left.language_name == right.language_name;
}

bool operator!=(const SyntaxSelection &left, const SyntaxSelection &right) {
    return !(left == right);
}

SyntaxSelection resolve_syntax_selection(const EditorConfig &config, const std::optional<std::string> &file_path) {
    if (config.syntax_name && !config.syntax_name->empty()) {
        if (*config.syntax_name == "none") {
            return {};
        }
        if (std::optional<const SyntaxLanguageConfig *> language = syntax_language_by_name(config, *config.syntax_name)) {
            return {SyntaxEngine::TreeSitter, (*language)->name};
        }
        throw std::runtime_error("unknown syntax: " + *config.syntax_name);
    }

    if (std::optional<const SyntaxLanguageConfig *> language = syntax_language_for_file(config, file_path)) {
        return {SyntaxEngine::TreeSitter, (*language)->name};
    }
    return {};
}

std::expected<std::vector<HighlightSpans>, std::string> highlight_document_syntax(
    const Lines &lines,
    const EditorConfig &config,
    const SyntaxSelection &selection) {
    switch (selection.engine) {
        case SyntaxEngine::None:
            return std::vector<HighlightSpans>(lines.size());
        case SyntaxEngine::TreeSitter:
            for (const SyntaxLanguageConfig &language : config.syntax_languages) {
                if (language.name == selection.language_name) {
                    return highlight_tree_sitter_document(lines, language);
                }
            }
            return std::unexpected("configured syntax language not found: " + selection.language_name);
    }
    return std::vector<HighlightSpans>(lines.size());
}

std::expected<std::vector<Range>, std::string> selection_ranges_for_document(
    const Lines &lines,
    const EditorConfig &config,
    const std::optional<std::string> &file_path,
    Position position) {
    const SyntaxLanguageConfig *language = matching_syntax_language(config, file_path);
    if (!language) {
        return std::unexpected("No local syntax tree");
    }
    return selection_ranges_tree_sitter_document(lines, *language, position);
}

std::string tree_sitter_status_summary(const EditorConfig &config, const std::optional<std::string> &file_path) {
    std::ostringstream status;
    status << "file: " << (file_path && !file_path->empty() ? *file_path : "(none)") << "\n";
    status << "configured languages: " << config.syntax_languages.size() << "\n";
    status << "syntax config: "
           << (config.syntax_config_path ? config.syntax_config_path->string() : "(default/none)") << "\n";

    SyntaxSelection selection;
    try {
        selection = resolve_syntax_selection(config, file_path);
    } catch (const std::exception &error) {
        status << "selection error: " << error.what();
        return status.str();
    }

    status << "selected engine: " << syntax_engine_name(selection.engine) << "\n";
    status << "selected language: " << (selection.language_name.empty() ? "(none)" : selection.language_name) << "\n";

    if (selection.engine != SyntaxEngine::TreeSitter) {
        status << "tree-sitter active: no";
        return status.str();
    }

    TreeSitterApi &api = tree_sitter_api();
    status << "runtime loaded: " << (api.loaded() ? "yes" : "no") << "\n";
    if (!api.loaded()) {
        status << "runtime error: " << api.error;
        return status.str();
    }

    std::optional<const SyntaxLanguageConfig *> language = syntax_language_by_name(config, selection.language_name);
    if (!language) {
        status << "language error: configured syntax language not found";
        return status.str();
    }

    status << "grammar path: " << (*language)->grammar_path.string() << "\n";
    status << "grammar symbol: " << (*language)->symbol_name << "\n";
    status << "highlights path: " << (*language)->highlights_path.string() << "\n";

    std::expected<LoadedTreeSitterLanguage *, std::string> loaded = load_tree_sitter_language(**language);
    if (!loaded) {
        status << "language loaded: no\n";
        status << "language error: " << loaded.error();
        return status.str();
    }

    status << "language loaded: yes\n";
    status << "capture count: " << (*loaded)->capture_names.size();
    return status.str();
}

std::string tree_sitter_health_summary(const EditorConfig &config) {
    std::ostringstream status;
    status << "syntax config: "
           << (config.syntax_config_path ? config.syntax_config_path->string() : "(default/none)") << "\n";
    status << "syntax chain: ";
    if (config.syntax_chain.empty()) {
        status << "(none)\n";
    } else {
        for (std::size_t index = 0; index < config.syntax_chain.size(); ++index) {
            if (index > 0) {
                status << " -> ";
            }
            status << config.syntax_chain[index].string();
        }
        status << "\n";
    }
    status << "configured languages: " << config.syntax_languages.size() << "\n";

    TreeSitterApi &api = tree_sitter_api();
    status << "runtime loaded: " << (api.loaded() ? "yes" : "no") << "\n";
    if (!api.loaded()) {
        status << "runtime error: " << api.error;
        return status.str();
    }

    for (const SyntaxLanguageConfig &language : config.syntax_languages) {
        status << "\n[" << language.name << "]\n";
        status << "grammar path: " << language.grammar_path.string() << "\n";
        status << "grammar exists: " << (std::filesystem::exists(language.grammar_path) ? "yes" : "no") << "\n";
        status << "highlights path: " << language.highlights_path.string() << "\n";
        status << "highlights exists: " << (std::filesystem::exists(language.highlights_path) ? "yes" : "no") << "\n";

        std::expected<LoadedTreeSitterLanguage *, std::string> loaded = load_tree_sitter_language(language);
        if (!loaded) {
            status << "loadable: no\n";
            status << "error: " << loaded.error() << "\n";
            continue;
        }
        status << "loadable: yes\n";
        status << "capture count: " << (*loaded)->capture_names.size() << "\n";
    }

    return status.str();
}

void invalidate_syntax_runtime_cache() {
    TreeSitterApi &api = tree_sitter_api();
    for (auto &[key, language] : language_cache()) {
        (void)key;
#if defined(__unix__) || defined(__APPLE__)
        if (language.query != nullptr && api.query_delete != nullptr) {
            api.query_delete(language.query);
        }
        if (language.parser != nullptr && api.parser_delete != nullptr) {
            api.parser_delete(language.parser);
        }
        if (language.grammar_handle != nullptr) {
            dlclose(language.grammar_handle);
        }
#endif
    }
    language_cache().clear();
}
