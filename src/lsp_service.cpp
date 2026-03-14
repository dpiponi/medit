#include "lsp_service.hpp"

#include "editor_commands.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "process_utils.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <ranges>
#include <sstream>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#if defined(__unix__) || defined(__APPLE__)
void ensure_sigpipe_ignored() {
    static std::once_flag once;
    std::call_once(once, []() {
        struct sigaction action {};
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        sigaction(SIGPIPE, &action, nullptr);
    });
}

bool is_broken_pipe_error(int error_code) {
    return error_code == EPIPE || error_code == ECONNRESET;
}

void close_fd_if_open(int &fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}
#endif

std::string json_escape(const std::string &text) {
    std::string escaped;
    for (char ch : text) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

std::string json_string(const std::string &text) {
    return "\"" + json_escape(text) + "\"";
}

Position position_from_lsp(const JsonValue &position_value, const EditorCore &core) {
    Utf16Position utf16;
    auto line = position_value.find("line");
    auto character = position_value.find("character");
    if (line != position_value.end() && line->is_number()) {
        utf16.row = static_cast<std::size_t>(line->get<double>());
    }
    if (character != position_value.end() && character->is_number()) {
        utf16.column = static_cast<std::size_t>(character->get<double>());
    }
    return core.position_for_utf16(utf16);
}

DiagnosticSeverity diagnostic_severity_from_lsp(const JsonValue &value) {
    if (!value.is_number()) {
        return DiagnosticSeverity::Warning;
    }
    return static_cast<int>(value.get<double>()) == 1 ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
}

std::size_t text_offset_for_position(const std::u32string &text, Position position) {
    std::size_t row = 0;
    std::size_t column = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (row == position.row && column == position.column) {
            return index;
        }
        if (text[index] == U'\n') {
            ++row;
            column = 0;
        } else {
            ++column;
        }
    }
    return text.size();
}

Utf16Position utf16_position_for_text_position(const std::u32string &text, Position position) {
    Utf16Position utf16{};
    std::size_t row = 0;
    std::size_t column = 0;
    for (char32_t codepoint : text) {
        if (row == position.row && column == position.column) {
            return utf16;
        }
        if (codepoint == U'\n') {
            ++row;
            column = 0;
            ++utf16.row;
            utf16.column = 0;
            continue;
        }
        ++column;
        utf16.column += codepoint > 0xFFFF ? 2 : 1;
    }
    return utf16;
}

Position text_position_for_utf16(const std::u32string &text, Utf16Position position) {
    std::size_t utf16_row = 0;
    std::size_t utf16_column = 0;
    std::size_t row = 0;
    std::size_t column = 0;
    for (char32_t codepoint : text) {
        if (utf16_row == position.row && utf16_column >= position.column) {
            return {row, column};
        }
        if (codepoint == U'\n') {
            if (utf16_row == position.row && utf16_column == position.column) {
                return {row, column};
            }
            ++row;
            column = 0;
            ++utf16_row;
            utf16_column = 0;
            continue;
        }
        std::size_t width = codepoint > 0xFFFF ? 2 : 1;
        if (utf16_row == position.row && utf16_column + width > position.column) {
            return {row, column};
        }
        ++column;
        utf16_column += width;
    }
    return {row, column};
}

void apply_incremental_text_change(std::u32string &document_text, Range range, const std::u32string &replacement) {
    Range normalized = normalized_range(range);
    std::size_t start_offset = text_offset_for_position(document_text, normalized.start);
    std::size_t end_offset = text_offset_for_position(document_text, normalized.end);
    document_text.replace(start_offset, end_offset - start_offset, replacement);
}

std::optional<std::pair<std::string, Position>> definition_location_from_result(const JsonValue &result) {
    const JsonValue *location = &result;
    if (result.is_array()) {
        if (result.empty()) {
            return std::nullopt;
        }
        location = &result.front();
    }

    if (!location->is_object()) {
        return std::nullopt;
    }

    auto extract_start_position = [](const std::string &document_uri, const JsonValue &range_object) -> std::optional<Position> {
        auto start = range_object.find("start");
        if (start == range_object.end() || !start->is_object()) {
            return std::nullopt;
        }
        EditorCore conversion_core;
        std::string path = file_path_from_uri(document_uri);
        if (!path.empty()) {
            conversion_core.load_file(path);
        }
        return position_from_lsp(*start, conversion_core);
    };

    auto uri = location->find("uri");
    if (uri != location->end() && uri->is_string()) {
        std::string normalized_uri = normalize_document_uri(uri->get<std::string>());
        auto range = location->find("range");
        if (range != location->end() && range->is_object()) {
            if (std::optional<Position> start = extract_start_position(normalized_uri, *range)) {
                return std::make_pair(normalized_uri, *start);
            }
        }
    }

    auto target_uri = location->find("targetUri");
    if (target_uri != location->end() && target_uri->is_string()) {
        std::string normalized_uri = normalize_document_uri(target_uri->get<std::string>());
        auto target_selection_range = location->find("targetSelectionRange");
        if (target_selection_range != location->end() && target_selection_range->is_object()) {
            if (std::optional<Position> start = extract_start_position(normalized_uri, *target_selection_range)) {
                return std::make_pair(normalized_uri, *start);
            }
        }
        auto target_range = location->find("targetRange");
        if (target_range != location->end() && target_range->is_object()) {
            if (std::optional<Position> start = extract_start_position(normalized_uri, *target_range)) {
                return std::make_pair(normalized_uri, *start);
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> hover_text_from_contents(const JsonValue &contents) {
    if (contents.is_string()) {
        return contents.get<std::string>();
    }
    if (contents.is_object()) {
        auto value = contents.find("value");
        if (value != contents.end() && value->is_string()) {
            return value->get<std::string>();
        }
        return std::nullopt;
    }
    if (contents.is_array()) {
        std::string combined;
        for (const JsonValue &item : contents) {
            std::optional<std::string> text = hover_text_from_contents(item);
            if (!text || text->empty()) {
                continue;
            }
            if (!combined.empty()) {
                combined += "\n\n";
            }
            combined += *text;
        }
        return combined.empty() ? std::nullopt : std::optional<std::string>(combined);
    }
    return std::nullopt;
}

std::vector<PopupMenuItem> completion_items_from_result(
    const JsonValue &result,
    const std::u32string &document_text,
    const ServiceRequest &request) {
    const JsonValue *items = &result;
    if (result.is_object()) {
        auto found = result.find("items");
        if (found == result.end() || !found->is_array()) {
            return {};
        }
        items = &*found;
    }
    if (!items->is_array()) {
        return {};
    }

    Range default_range = request.completion_range.value_or(
        Range{text_position_for_utf16(document_text, request.utf16_position),
              text_position_for_utf16(document_text, request.utf16_position)});
    std::string prefix = request.completion_prefix;
    std::vector<PopupMenuItem> parsed;
    for (const JsonValue &item : *items) {
        if (!item.is_object()) {
            continue;
        }
        auto label = item.find("label");
        if (label == item.end() || !label->is_string()) {
            continue;
        }
        PopupMenuItem parsed_item;
        parsed_item.label = label->get<std::string>();
        parsed_item.insert_text = parsed_item.label;
        parsed_item.replace_range = default_range;
        std::string filter_text = parsed_item.label;

        auto detail = item.find("detail");
        if (detail != item.end() && detail->is_string()) {
            parsed_item.detail = detail->get<std::string>();
        }

        auto filter = item.find("filterText");
        if (filter != item.end() && filter->is_string()) {
            filter_text = filter->get<std::string>();
        }

        auto insert_text = item.find("insertText");
        if (insert_text != item.end() && insert_text->is_string()) {
            parsed_item.insert_text = insert_text->get<std::string>();
        }

        auto text_edit = item.find("textEdit");
        if (text_edit != item.end() && text_edit->is_object()) {
            auto new_text = text_edit->find("newText");
            if (new_text != text_edit->end() && new_text->is_string()) {
                parsed_item.insert_text = new_text->get<std::string>();
            }
        }

        if (!prefix.empty()) {
            std::string candidate = !parsed_item.insert_text.empty() ? parsed_item.insert_text : parsed_item.label;
            if (!candidate.starts_with(prefix) && !parsed_item.label.starts_with(prefix) && !filter_text.starts_with(prefix)) {
                continue;
            }
        }

        parsed.push_back(std::move(parsed_item));
        if (parsed.size() >= 32) {
            break;
        }
    }
    log_debug(
        "completion parsed uri-prefix=[" + prefix + "] count=" + std::to_string(parsed.size()) +
        " total=" + std::to_string(items->size()));
    return parsed;
}

std::vector<Range> selection_ranges_from_result(const JsonValue &result, const EditorCore &core) {
    if (!result.is_array() || result.empty()) {
        return {};
    }
    const JsonValue &first = result.front();
    if (!first.is_object()) {
        return {};
    }

    std::vector<Range> ranges;
    const JsonValue *current = &first;
    while (current != nullptr && current->is_object()) {
        auto range_value = current->find("range");
        if (range_value != current->end() && range_value->is_object()) {
            auto start = range_value->find("start");
            auto end = range_value->find("end");
            if (start != range_value->end() && end != range_value->end() && start->is_object() && end->is_object()) {
                Range range{position_from_lsp(*start, core), position_from_lsp(*end, core)};
                if (!positions_equal(range.start, range.end)) {
                    ranges.push_back(normalized_range(range));
                }
            }
        }

        auto parent = current->find("parent");
        if (parent == current->end() || !parent->is_object()) {
            break;
        }
        current = &*parent;
    }

    ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const Range &left, const Range &right) {
        return positions_equal(left.start, right.start) && positions_equal(left.end, right.end);
    }), ranges.end());
    return ranges;
}

}  // namespace

std::string encode_lsp_message(const std::string &payload) {
    std::ostringstream message;
    message << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
    return message.str();
}

std::vector<std::string> extract_lsp_messages(std::string &buffer) {
    std::vector<std::string> messages;
    while (true) {
        std::size_t header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            break;
        }
        std::size_t content_length = 0;
        std::string headers = buffer.substr(0, header_end);
        std::istringstream header_stream(headers);
        std::string line;
        while (std::getline(header_stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.starts_with("Content-Length:")) {
                content_length = static_cast<std::size_t>(std::stoul(line.substr(std::strlen("Content-Length:"))));
            }
        }
        std::size_t message_start = header_end + 4;
        if (buffer.size() < message_start + content_length) {
            break;
        }
        messages.push_back(buffer.substr(message_start, content_length));
        buffer.erase(0, message_start + content_length);
    }
    return messages;
}

void close_lsp_launch_pipes(
    std::array<int, 2> &stdin_pipe,
    std::array<int, 2> &stdout_pipe,
    std::array<int, 2> &stderr_pipe) {
#if defined(__unix__) || defined(__APPLE__)
    close_fd_if_open(stdin_pipe[0]);
    close_fd_if_open(stdin_pipe[1]);
    close_fd_if_open(stdout_pipe[0]);
    close_fd_if_open(stdout_pipe[1]);
    close_fd_if_open(stderr_pipe[0]);
    close_fd_if_open(stderr_pipe[1]);
#else
    (void)stdin_pipe;
    (void)stdout_pipe;
    (void)stderr_pipe;
#endif
}

LspService::LspService(LspServerConfig config) : config_(std::move(config)) {}

LspService::~LspService() {
    stop();
}

std::string LspService::name() const {
    return "lsp:" + config_.name;
}

void LspService::start() {
    if (running_ || config_.command.empty()) {
        return;
    }
#if defined(__unix__) || defined(__APPLE__)
    ensure_sigpipe_ignored();
    if (std::optional<std::string> missing = missing_executable_in_command(config_.command)) {
        queue_status("LSP executable not found: " + *missing);
        log_debug("external command kind=lsp-server missing executable=" + *missing + " command=" + config_.command);
        return;
    }
    if (!spawn_process()) {
        queue_status("LSP start failed");
        return;
    }
    stopping_ = false;
    running_ = true;
    reader_thread_ = std::thread(&LspService::reader_loop, this);
    stderr_thread_ = std::thread(&LspService::stderr_loop, this);
#else
    queue_status("LSP transport unsupported on this platform");
#endif
}

void LspService::stop() {
    if (!running_) {
        return;
    }
    stopping_ = true;
    shutdown_process();
    running_ = false;
    initialized_ = false;
    initialize_request_id_ = -1;
    workspace_root_.reset();
    open_documents_.clear();
    document_texts_.clear();
    pending_document_changes_.clear();
    pending_change_times_.clear();
    pending_document_texts_.clear();
    pending_requests_.clear();
}

void LspService::handle_editor_event(const EditorEvent &event) {
    if (!running_) {
        return;
    }
    if (!matches_document(event.document_uri) && open_documents_.find(event.document_uri) == open_documents_.end()) {
        return;
    }
    switch (event.type) {
        case EditorEventType::DocumentOpened:
            if (!initialized_) {
                ensure_initialized_for_event(event);
                pending_editor_events_.push_back(event);
                return;
            }
            flush_pending_document_changes(true, event.document_uri);
            pending_document_changes_.erase(event.document_uri);
            pending_change_times_.erase(event.document_uri);
            pending_document_texts_.erase(event.document_uri);
            send_editor_event(event);
            return;
        case EditorEventType::DocumentChanged:
            if (!initialized_) {
                ensure_initialized_for_event(event);
                pending_editor_events_.push_back(event);
                return;
            }
            {
                std::u32string latest_text;
                auto pending_text = pending_document_texts_.find(event.document_uri);
                if (pending_text != pending_document_texts_.end()) {
                    latest_text = pending_text->second;
                } else {
                    auto existing_text = document_texts_.find(event.document_uri);
                    if (existing_text != document_texts_.end()) {
                        latest_text = existing_text->second;
                    }
                }

                if (event.range) {
                    apply_incremental_text_change(latest_text, *event.range, event.text);
                } else {
                    latest_text = event.text;
                }

                pending_document_texts_[event.document_uri] = latest_text;
                EditorEvent coalesced = event;
                coalesced.range.reset();
                coalesced.text = latest_text;
                pending_document_changes_[event.document_uri] = std::move(coalesced);
            }
            pending_change_times_[event.document_uri] = std::chrono::steady_clock::now();
            return;
        case EditorEventType::DocumentSaved:
        case EditorEventType::DocumentClosed:
            if (!initialized_) {
                ensure_initialized_for_event(event);
                pending_editor_events_.push_back(event);
                return;
            }
            flush_pending_document_changes(true, event.document_uri);
            send_editor_event(event);
            return;
        default:
            return;
    }
}

void LspService::handle_request(const ServiceRequest &request) {
    if (!running_) {
        return;
    }
    if (!matches_document(request.document_uri)) {
        return;
    }
    if (!initialized_ || open_documents_.find(request.document_uri) == open_documents_.end()) {
        if (request.type == ServiceRequestType::GoToDefinition) {
            queue_status("Definition unavailable");
        } else if (request.type == ServiceRequestType::Hover) {
            queue_status("Hover unavailable");
        } else if (request.type == ServiceRequestType::Completion) {
            queue_status("Completion unavailable");
        } else if (request.type == ServiceRequestType::SelectionRange) {
            queue_status("Selection range unavailable");
        }
        return;
    }
    flush_pending_document_changes(true, request.document_uri);
    if (request.type == ServiceRequestType::GoToDefinition) {
        send_definition_request(request);
    } else if (request.type == ServiceRequestType::Hover || request.type == ServiceRequestType::WarmHover) {
        send_hover_request(request);
    } else if (request.type == ServiceRequestType::Completion) {
        send_completion_request(request);
    } else if (request.type == ServiceRequestType::SelectionRange) {
        send_selection_range_request(request);
    }
}

void LspService::send_editor_event(const EditorEvent &event) {
    switch (event.type) {
        case EditorEventType::DocumentOpened:
            send_did_open(event);
            break;
        case EditorEventType::DocumentChanged:
            send_did_change(event);
            break;
        case EditorEventType::DocumentSaved:
            send_did_save(event);
            break;
        case EditorEventType::DocumentClosed:
            send_did_close(event);
            break;
        default:
            break;
    }
}

void LspService::flush_pending_editor_events() {
    std::vector<EditorEvent> events = std::move(pending_editor_events_);
    pending_editor_events_.clear();
    for (const EditorEvent &event : events) {
        send_editor_event(event);
    }
}

void LspService::flush_pending_document_changes(bool force, const std::optional<std::string> &document_uri) {
    if (!initialized_) {
        return;
    }

    constexpr auto kDidChangeDebounce = std::chrono::milliseconds(125);
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> ready_documents;
    ready_documents.reserve(pending_document_changes_.size());

    for (const auto &[uri, event] : pending_document_changes_) {
        (void)event;
        if (document_uri && uri != *document_uri) {
            continue;
        }
        auto found = pending_change_times_.find(uri);
        if (force || found == pending_change_times_.end() || now - found->second >= kDidChangeDebounce) {
            ready_documents.push_back(uri);
        }
    }

    for (const std::string &uri : ready_documents) {
        auto found = pending_document_changes_.find(uri);
        if (found == pending_document_changes_.end()) {
            continue;
        }
        send_did_change(found->second);
        pending_document_changes_.erase(found);
        pending_change_times_.erase(uri);
        pending_document_texts_.erase(uri);
    }
}

std::vector<ServiceEvent> LspService::poll() {
    flush_pending_document_changes(false);
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceEvent> events = std::move(pending_events_);
    pending_events_.clear();
    return events;
}

std::optional<int> LspService::poll_interval_ms() const {
    return 10;
}

std::string LspService::status_summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream summary;
    summary << name()
            << "\ncommand: " << (config_.command.empty() ? "<none>" : config_.command)
            << "\nrunning: " << (running_ ? "yes" : "no")
            << "\ninitialized: " << (initialized_ ? "yes" : "no");
    if (workspace_root_) {
        summary << "\nworkspace: " << workspace_root_->string();
    } else {
        summary << "\nworkspace: <unset>";
    }
    summary << "\nopen documents: " << open_documents_.size();
    summary << "\npending requests: " << pending_requests_.size();
    summary << "\npending changes: " << pending_document_changes_.size();
    if (!last_status_message_.empty()) {
        summary << "\nlast status: " << last_status_message_;
    }
    if (!last_stderr_line_.empty()) {
        summary << "\nlast stderr: " << last_stderr_line_;
    }
    return summary.str();
}

void LspService::queue_event(ServiceEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.push_back(std::move(event));
}

void LspService::queue_status(const std::string &message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_status_message_ = message;
    }
    EditorCommand command;
    command.type = EditorCommandType::SetStatusMessage;
    command.message = message;
    queue_event({ServiceEventType::Notification, name(), "status", command, std::nullopt, 0, std::nullopt, U""});
}

void LspService::queue_stderr_line(const std::string &line) {
    if (line.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_stderr_line_ = line;
    }
    queue_status("LSP stderr: " + line);
}

bool LspService::matches_document(const std::string &document_uri) const {
    if (config_.patterns.empty()) {
        return false;
    }
    if (std::ranges::find(config_.patterns, "*") != config_.patterns.end()) {
        return true;
    }
    std::string file_path = file_path_from_uri(document_uri);
    if (file_path.empty()) {
        return false;
    }
    return std::ranges::find_if(config_.patterns, [&file_path](const std::string &pattern) {
        return file_path_matches_glob(file_path, pattern);
    }) != config_.patterns.end();
}

void LspService::ensure_initialized_for_event(const EditorEvent &event) {
    if (initialize_request_id_ >= 0) {
        return;
    }
    std::string file_path = file_path_from_uri(event.document_uri);
    workspace_root_ = infer_workspace_root(
        config_,
        file_path.empty() ? std::optional<std::string>() : std::optional<std::string>(file_path));
    send_initialize(*workspace_root_);
}

#if defined(__unix__) || defined(__APPLE__)
bool LspService::spawn_process() {
    std::array<int, 2> stdin_pipe{-1, -1};
    std::array<int, 2> stdout_pipe{-1, -1};
    std::array<int, 2> stderr_pipe{-1, -1};
    if (pipe(stdin_pipe.data()) != 0 || pipe(stdout_pipe.data()) != 0 || pipe(stderr_pipe.data()) != 0) {
        close_lsp_launch_pipes(stdin_pipe, stdout_pipe, stderr_pipe);
        log_debug("external command kind=lsp-server pipe-setup failed command=" + config_.command);
        return false;
    }

    log_debug("external command kind=lsp-server spawn command=" + config_.command);
    int pid = fork();
    if (pid < 0) {
        close_lsp_launch_pipes(stdin_pipe, stdout_pipe, stderr_pipe);
        log_debug("external command kind=lsp-server fork failed command=" + config_.command);
        return false;
    }
    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        execl("/bin/sh", "sh", "-lc", config_.command.c_str(), nullptr);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    child_pid_ = pid;
    log_debug("external command kind=lsp-server started pid=" + std::to_string(child_pid_) + " command=" + config_.command);
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    stderr_fd_ = stderr_pipe[0];
    fcntl(stdout_fd_, F_SETFL, fcntl(stdout_fd_, F_GETFL, 0) | O_NONBLOCK);
    fcntl(stderr_fd_, F_SETFL, fcntl(stderr_fd_, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

void LspService::shutdown_process() {
    int pid = child_pid_;
    if (stdin_fd_ >= 0) {
        close(stdin_fd_);
        stdin_fd_ = -1;
    }
    if (pid > 0) {
        auto wait_for_exit = [pid](int attempts, std::chrono::milliseconds delay) {
            for (int attempt = 0; attempt < attempts; ++attempt) {
                int status = 0;
                pid_t result = waitpid(pid, &status, WNOHANG);
                if (result == pid || result < 0) {
                    return true;
                }
                std::this_thread::sleep_for(delay);
            }
            return false;
        };

        bool exited = wait_for_exit(2, std::chrono::milliseconds(10));
        if (!exited) {
            kill(pid, SIGTERM);
            exited = wait_for_exit(10, std::chrono::milliseconds(10));
        }
        if (!exited) {
            kill(pid, SIGKILL);
            exited = wait_for_exit(10, std::chrono::milliseconds(10));
        }
        if (!exited) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }
    if (stdout_fd_ >= 0) {
        close(stdout_fd_);
        stdout_fd_ = -1;
    }
    if (stderr_fd_ >= 0) {
        close(stderr_fd_);
        stderr_fd_ = -1;
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (stderr_thread_.joinable()) {
        stderr_thread_.join();
    }
    if (pid > 0) {
        child_pid_ = -1;
    }
}

void LspService::reader_loop() {
    char chunk[4096];
    while (stdout_fd_ >= 0) {
        ssize_t read_count = read(stdout_fd_, chunk, sizeof(chunk));
        if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stopping_) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (read_count <= 0) {
            break;
        }
        read_buffer_.append(chunk, static_cast<std::size_t>(read_count));
        for (const std::string &payload : extract_lsp_messages(read_buffer_)) {
            handle_message(payload);
        }
    }
    if (!stopping_ && !initialized_) {
        queue_status("LSP exited before initialize");
    }
}

void LspService::stderr_loop() {
    char chunk[4096];
    while (stderr_fd_ >= 0) {
        ssize_t read_count = read(stderr_fd_, chunk, sizeof(chunk));
        if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stopping_) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (read_count <= 0) {
            break;
        }
        stderr_buffer_.append(chunk, static_cast<std::size_t>(read_count));
        std::size_t line_end = 0;
        while ((line_end = stderr_buffer_.find('\n')) != std::string::npos) {
            std::string line = stderr_buffer_.substr(0, line_end);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            stderr_buffer_.erase(0, line_end + 1);
            queue_stderr_line(line);
        }
    }
    if (!stderr_buffer_.empty()) {
        queue_stderr_line(stderr_buffer_);
        stderr_buffer_.clear();
    }
}

bool LspService::write_payload(const std::string &payload) {
    if (stdin_fd_ < 0) {
        if (!stopping_) {
            queue_status("LSP write failed");
        }
        return false;
    }
    std::string framed = encode_lsp_message(payload);
    const char *data = framed.data();
    std::size_t remaining = framed.size();
    while (remaining > 0) {
        ssize_t wrote = write(stdin_fd_, data, remaining);
        if (wrote <= 0) {
#if defined(__unix__) || defined(__APPLE__)
            int error_code = errno;
            if (is_broken_pipe_error(error_code)) {
                close(stdin_fd_);
                stdin_fd_ = -1;
                pending_requests_.clear();
                pending_document_changes_.clear();
                pending_change_times_.clear();
                pending_document_texts_.clear();
                if (!stopping_) {
                    queue_status("LSP pipe closed");
                }
                return false;
            }
#endif
            if (!stopping_) {
                queue_status("LSP write failed");
            }
            return false;
        }
        data += wrote;
        remaining -= static_cast<std::size_t>(wrote);
    }
    return true;
}
#else
bool LspService::spawn_process() { return false; }
void LspService::shutdown_process() {}
void LspService::reader_loop() {}
bool LspService::write_payload(const std::string &) { return false; }
#endif

void LspService::send_initialize(const std::filesystem::path &workspace_root) {
    initialize_request_id_ = next_request_id_++;
    std::string root_uri = file_uri_for_path(workspace_root.string());
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"id\":" << initialize_request_id_ << ","
            << "\"method\":\"initialize\","
            << "\"params\":{"
            << "\"processId\":null,"
            << "\"rootUri\":" << json_string(root_uri) << ","
            << "\"capabilities\":{},"
            << "\"clientInfo\":{\"name\":\"medit\"}"
            << "}"
            << "}";
    if (!write_payload(payload.str())) {
        queue_status("LSP initialize failed");
    }
}

void LspService::send_initialized() {
    write_payload("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
}

void LspService::send_shutdown() {
    std::ostringstream payload;
    payload << "{\"jsonrpc\":\"2.0\",\"id\":" << next_request_id_++ << ",\"method\":\"shutdown\",\"params\":null}";
    write_payload(payload.str());
}

void LspService::send_exit() {
    write_payload("{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");
}

void LspService::send_did_open(const EditorEvent &event) {
    if (!initialized_) {
        return;
    }
    open_documents_.insert(event.document_uri);
    document_texts_[event.document_uri] = event.text;
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"method\":\"textDocument/didOpen\","
            << "\"params\":{\"textDocument\":{"
            << "\"uri\":" << json_string(event.document_uri) << ","
            << "\"languageId\":" << json_string(config_.language_id) << ","
            << "\"version\":" << event.document_version << ","
            << "\"text\":" << json_string(u32_to_utf8(event.text))
            << "}}}";
    write_payload(payload.str());

    ServiceRequest warmup;
    warmup.type = ServiceRequestType::WarmHover;
    warmup.document_uri = event.document_uri;
    warmup.utf16_position = Utf16Position{0, 0};
    warmup.document_version = event.document_version;
    send_hover_request(warmup);
}

void LspService::send_did_change(const EditorEvent &event) {
    if (!initialized_ || open_documents_.find(event.document_uri) == open_documents_.end()) {
        return;
    }
    auto found = document_texts_.find(event.document_uri);
    std::u32string *document_text = found != document_texts_.end() ? &found->second : nullptr;
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"method\":\"textDocument/didChange\","
            << "\"params\":{"
            << "\"textDocument\":{\"uri\":" << json_string(event.document_uri) << ",\"version\":" << event.document_version << "},"
            << "\"contentChanges\":[";
    if (event.range && document_text != nullptr) {
        Range normalized = normalized_range(*event.range);
        Utf16Position start = utf16_position_for_text_position(*document_text, normalized.start);
        Utf16Position end = utf16_position_for_text_position(*document_text, normalized.end);
        payload << "{"
                << "\"range\":{"
                << "\"start\":{\"line\":" << start.row << ",\"character\":" << start.column << "},"
                << "\"end\":{\"line\":" << end.row << ",\"character\":" << end.column << "}"
                << "},"
                << "\"text\":" << json_string(u32_to_utf8(event.text))
                << "}";
        apply_incremental_text_change(*document_text, normalized, event.text);
    } else {
        payload << "{\"text\":" << json_string(u32_to_utf8(event.text)) << "}";
        document_texts_[event.document_uri] = event.text;
    }
    payload << "]"
            << "}}";
    write_payload(payload.str());
}

void LspService::send_did_save(const EditorEvent &event) {
    if (!initialized_ || open_documents_.find(event.document_uri) == open_documents_.end()) {
        return;
    }
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"method\":\"textDocument/didSave\","
            << "\"params\":{\"textDocument\":{\"uri\":" << json_string(event.document_uri) << "}}"
            << "}";
    write_payload(payload.str());
}

void LspService::send_did_close(const EditorEvent &event) {
    if (!initialized_ || open_documents_.find(event.document_uri) == open_documents_.end()) {
        return;
    }
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"method\":\"textDocument/didClose\","
            << "\"params\":{\"textDocument\":{\"uri\":" << json_string(event.document_uri) << "}}"
            << "}";
    write_payload(payload.str());
    open_documents_.erase(event.document_uri);
    document_texts_.erase(event.document_uri);
}

void LspService::send_definition_request(const ServiceRequest &request) {
    int request_id = next_request_id_++;
    pending_requests_[request_id] = request;
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"id\":" << request_id << ","
            << "\"method\":\"textDocument/definition\","
            << "\"params\":{"
            << "\"textDocument\":{\"uri\":" << json_string(request.document_uri) << "},"
            << "\"position\":{\"line\":" << request.utf16_position.row
            << ",\"character\":" << request.utf16_position.column << "}"
            << "}"
            << "}";
    if (!write_payload(payload.str())) {
        pending_requests_.erase(request_id);
        queue_status("Definition request failed");
    }
}

void LspService::send_hover_request(const ServiceRequest &request) {
    int request_id = next_request_id_++;
    pending_requests_[request_id] = request;
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"id\":" << request_id << ","
            << "\"method\":\"textDocument/hover\","
            << "\"params\":{"
            << "\"textDocument\":{\"uri\":" << json_string(request.document_uri) << "},"
            << "\"position\":{\"line\":" << request.utf16_position.row
            << ",\"character\":" << request.utf16_position.column << "}"
            << "}"
            << "}";
    if (!write_payload(payload.str())) {
        pending_requests_.erase(request_id);
        queue_status("Hover request failed");
    }
}

void LspService::send_completion_request(const ServiceRequest &request) {
    int request_id = next_request_id_++;
    pending_requests_[request_id] = request;
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"id\":" << request_id << ","
            << "\"method\":\"textDocument/completion\","
            << "\"params\":{"
            << "\"textDocument\":{\"uri\":" << json_string(request.document_uri) << "},"
            << "\"position\":{\"line\":" << request.utf16_position.row
            << ",\"character\":" << request.utf16_position.column << "}"
            << "}"
            << "}";
    if (!write_payload(payload.str())) {
        pending_requests_.erase(request_id);
        queue_status("Completion request failed");
    }
}

void LspService::send_selection_range_request(const ServiceRequest &request) {
    int request_id = next_request_id_++;
    pending_requests_[request_id] = request;
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"id\":" << request_id << ","
            << "\"method\":\"textDocument/selectionRange\","
            << "\"params\":{"
            << "\"textDocument\":{\"uri\":" << json_string(request.document_uri) << "},"
            << "\"positions\":[{\"line\":" << request.utf16_position.row
            << ",\"character\":" << request.utf16_position.column << "}]"
            << "}"
            << "}";
    if (!write_payload(payload.str())) {
        pending_requests_.erase(request_id);
        queue_status("Selection range request failed");
    }
}

void LspService::handle_message(const std::string &payload) {
    JsonValue root = parse_json(payload);
    if (!root.is_object()) {
        return;
    }

    auto id = root.find("id");
    if (id != root.end() && id->is_number() && static_cast<int>(id->get<double>()) == initialize_request_id_) {
        auto error = root.find("error");
        if (error != root.end() && error->is_object()) {
            auto message = error->find("message");
            if (message != error->end() && message->is_string()) {
                queue_status("LSP initialize error: " + message->get<std::string>());
            } else {
                queue_status("LSP initialize error");
            }
            return;
        }
        initialized_ = true;
        send_initialized();
        flush_pending_editor_events();
        queue_status("LSP initialized");
        return;
    }

    if (id != root.end() && id->is_number()) {
        int request_id = static_cast<int>(id->get<double>());
        auto pending = pending_requests_.find(request_id);
        if (pending != pending_requests_.end()) {
            ServiceRequest request = pending->second;
            pending_requests_.erase(pending);

            auto error = root.find("error");
            if (error != root.end() && !error->is_null()) {
                if (request.type == ServiceRequestType::GoToDefinition) {
                    queue_status("Definition request failed");
                } else if (request.type == ServiceRequestType::Hover) {
                    queue_status("Hover request failed");
                } else if (request.type == ServiceRequestType::Completion) {
                    queue_status("Completion request failed");
                } else if (request.type == ServiceRequestType::SelectionRange) {
                    queue_status("Selection range request failed");
                }
                return;
            }

            auto result = root.find("result");
            if (request.type == ServiceRequestType::GoToDefinition) {
                if (result == root.end() || result->is_null()) {
                    queue_status("Definition not found");
                    return;
                }

                std::optional<std::pair<std::string, Position>> location = definition_location_from_result(*result);
                if (!location) {
                    queue_status("Definition not found");
                    return;
                }

                EditorCommand command;
                command.type = EditorCommandType::OpenLocation;
                command.document_uri = location->first;
                command.position = location->second;
                queue_event({ServiceEventType::Notification, name(), "definition", command, location->first, 0, std::nullopt, U""});
            } else if (request.type == ServiceRequestType::Hover) {
                if (result == root.end() || result->is_null()) {
                    queue_status("No hover information");
                    return;
                }
                if (!result->is_object()) {
                    queue_status("No hover information");
                    return;
                }
                auto contents = result->find("contents");
                if (contents == result->end()) {
                    queue_status("No hover information");
                    return;
                }
                std::optional<std::string> hover_text = hover_text_from_contents(*contents);
                if (!hover_text || hover_text->empty()) {
                    queue_status("No hover information");
                    return;
                }
                EditorCommand command;
                command.type = EditorCommandType::ShowPopup;
                command.title = "Hover";
                command.message = *hover_text;
                command.document_uri = request.document_uri;
                queue_event({ServiceEventType::Notification, name(), "hover", command, request.document_uri, 0, std::nullopt, U""});
            } else if (request.type == ServiceRequestType::Completion) {
                if (result == root.end() || result->is_null()) {
                    queue_status("No completions");
                    return;
                }
                std::u32string document_text;
                auto pending_text = pending_document_texts_.find(request.document_uri);
                if (pending_text != pending_document_texts_.end()) {
                    document_text = pending_text->second;
                } else {
                    auto found = document_texts_.find(request.document_uri);
                    if (found != document_texts_.end()) {
                        document_text = found->second;
                    }
                }
                std::vector<PopupMenuItem> items = completion_items_from_result(*result, document_text, request);
                if (items.empty()) {
                    queue_status("No completions");
                    return;
                }
                EditorCommand command;
                command.type = EditorCommandType::ShowPopup;
                command.title = "Completion";
                command.popup_kind = PopupKind::Menu;
                command.popup_items = std::move(items);
                command.document_uri = request.document_uri;
                command.position = text_position_for_utf16(document_text, request.utf16_position);
                queue_event(
                    {ServiceEventType::Notification, name(), "completion", command, request.document_uri, request.document_version, std::nullopt, U""});
            } else if (request.type == ServiceRequestType::SelectionRange) {
                if (result == root.end() || result->is_null()) {
                    queue_status("No enclosing AST range");
                    return;
                }
                EditorCore conversion_core;
                std::string file_path = file_path_from_uri(request.document_uri);
                if (!file_path.empty()) {
                    conversion_core.load_file(file_path);
                }
                std::vector<Range> ranges = selection_ranges_from_result(*result, conversion_core);
                if (ranges.empty()) {
                    queue_status("No enclosing AST range");
                    return;
                }
                EditorCommand command;
                command.type = EditorCommandType::SetSelectionRange;
                command.document_uri = request.document_uri;
                command.selection_range = ranges.front();
                command.selection_ranges = std::move(ranges);
                command.position = conversion_core.position_for_utf16(request.utf16_position);
                queue_event({ServiceEventType::Notification, name(), "selection_range", command, request.document_uri, request.document_version, std::nullopt, U""});
            } else if (request.type == ServiceRequestType::WarmHover) {
                return;
            }
            return;
        }
    }

    auto method = root.find("method");
    if (method == root.end() || !method->is_string()) {
        return;
    }

    std::string method_name = method->get<std::string>();
    if (method_name == "window/logMessage" || method_name == "window/showMessage") {
        auto params = root.find("params");
        if (params != root.end() && params->is_object()) {
            auto message = params->find("message");
            if (message != params->end() && message->is_string()) {
                queue_status("LSP: " + message->get<std::string>());
            }
        }
        return;
    }

    if (method_name != "textDocument/publishDiagnostics") {
        return;
    }

    auto params = root.find("params");
    if (params == root.end() || !params->is_object()) {
        return;
    }

    auto uri = params->find("uri");
    auto diagnostics = params->find("diagnostics");
    if (uri == params->end() || diagnostics == params->end() || !uri->is_string() || !diagnostics->is_array()) {
        return;
    }

    std::string normalized_uri = normalize_document_uri(uri->get<std::string>());
    std::vector<Diagnostic> parsed_diagnostics;
    EditorCore conversion_core;
    std::string file_path = file_path_from_uri(normalized_uri);
    if (!file_path.empty()) {
        conversion_core.load_file(file_path);
    }
    for (const JsonValue &diagnostic_value : *diagnostics) {
        if (!diagnostic_value.is_object()) {
            continue;
        }
        auto range = diagnostic_value.find("range");
        auto message = diagnostic_value.find("message");
        if (range == diagnostic_value.end() || message == diagnostic_value.end() || !range->is_object() || !message->is_string()) {
            continue;
        }
        auto start = range->find("start");
        auto end = range->find("end");
        if (start == range->end() || end == range->end() || !start->is_object() || !end->is_object()) {
            continue;
        }
        Diagnostic parsed;
        parsed.range = {position_from_lsp(*start, conversion_core), position_from_lsp(*end, conversion_core)};
        auto severity = diagnostic_value.find("severity");
        if (severity != diagnostic_value.end()) {
            parsed.severity = diagnostic_severity_from_lsp(*severity);
        }
        auto source = diagnostic_value.find("source");
        if (source != diagnostic_value.end() && source->is_string()) {
            parsed.source = source->get<std::string>();
        }
        parsed.message = utf8_to_u32(message->get<std::string>());
        parsed_diagnostics.push_back(std::move(parsed));
    }

    EditorCommand command;
    command.type = EditorCommandType::SetDiagnostics;
    command.document_uri = normalized_uri;
    command.diagnostics = std::move(parsed_diagnostics);
    queue_event({ServiceEventType::Notification, name(), "publishDiagnostics", command, normalized_uri, 0, std::nullopt, U""});
}
