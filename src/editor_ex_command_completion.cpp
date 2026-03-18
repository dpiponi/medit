#include "editor_ex_command_completion.hpp"

#include "string_utils.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace {

bool path_has_trailing_separator(std::string_view path) {
    return !path.empty() && (path.back() == '/' || path.back() == '\\');
}

struct FileCompletionContext {
    std::filesystem::path search_directory;
    std::string candidate_prefix;
    std::string initial_filter;
};

std::optional<FileCompletionContext> edit_file_completion_context(std::string_view command) {
    std::size_t separator = command.find_first_of(" \t");
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view verb = command.substr(0, separator);
    if (verb != "e") {
        return std::nullopt;
    }

    std::size_t argument_start = command.find_first_not_of(" \t", separator);
    std::string raw_argument =
        argument_start == std::string_view::npos ? std::string() : std::string(command.substr(argument_start));
    std::string expanded_argument = expand_user_path(raw_argument);

    FileCompletionContext context;
    context.initial_filter = raw_argument;
    if (raw_argument.empty()) {
        context.search_directory = std::filesystem::current_path();
        return context;
    }

    if (path_has_trailing_separator(raw_argument)) {
        context.search_directory = std::filesystem::path(expanded_argument);
        context.candidate_prefix = raw_argument;
        return context;
    }

    std::filesystem::path expanded_path(expanded_argument);
    std::filesystem::path expanded_parent = expanded_path.parent_path();
    context.search_directory = expanded_parent.empty() ? std::filesystem::current_path() : expanded_parent;

    std::filesystem::path raw_parent = std::filesystem::path(raw_argument).parent_path();
    context.candidate_prefix = raw_parent.string();
    if (!context.candidate_prefix.empty() && !path_has_trailing_separator(context.candidate_prefix)) {
        context.candidate_prefix.push_back(std::filesystem::path::preferred_separator);
    }
    return context;
}

}  // namespace

std::optional<EditFileCompletionResult> complete_edit_file_command(std::string_view command) {
    std::optional<FileCompletionContext> context = edit_file_completion_context(command);
    if (!context) {
        return std::nullopt;
    }

    std::error_code status_error;
    if (!std::filesystem::is_directory(context->search_directory, status_error) || status_error) {
        return EditFileCompletionResult{context->initial_filter, {}};
    }

    struct CompletionCandidate {
        std::string label;
        std::string detail;
        bool directory = false;
    };

    std::vector<CompletionCandidate> candidates;
    std::error_code iteration_error;
    for (std::filesystem::directory_iterator it(
             context->search_directory,
             std::filesystem::directory_options::skip_permission_denied,
             iteration_error);
         !iteration_error && it != std::filesystem::directory_iterator();
         it.increment(iteration_error)) {
        const std::filesystem::directory_entry &entry = *it;
        std::error_code entry_error;
        bool is_directory = entry.is_directory(entry_error);
        if (entry_error) {
            continue;
        }

        std::string label = context->candidate_prefix + entry.path().filename().string();
        if (is_directory) {
            label.push_back('/');
        }
        candidates.push_back({std::move(label), is_directory ? "directory" : "file", is_directory});
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const CompletionCandidate &left, const CompletionCandidate &right) {
            if (left.directory != right.directory) {
                return left.directory > right.directory;
            }
            return left.label < right.label;
        });

    PopupMenuItems items;
    items.reserve(candidates.size());
    for (const CompletionCandidate &candidate : candidates) {
        items.push_back({candidate.label, candidate.detail, "e " + candidate.label, std::nullopt});
    }
    return EditFileCompletionResult{context->initial_filter, std::move(items)};
}
