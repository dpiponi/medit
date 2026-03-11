#include "lsp_service.hpp"

#include "editor_commands.hpp"
#include "json.hpp"

#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

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

std::string file_path_from_uri(const std::string &uri) {
    if (uri.rfind("file://", 0) == 0) {
        return uri.substr(7);
    }
    return "";
}

Position position_from_lsp(const JsonValue &position_value, const EditorCore &core) {
    Utf16Position utf16;
    auto line = position_value.object_value.find("line");
    auto character = position_value.object_value.find("character");
    if (line != position_value.object_value.end() && line->second.type == JsonValue::Type::Number) {
        utf16.row = static_cast<std::size_t>(line->second.number_value);
    }
    if (character != position_value.object_value.end() && character->second.type == JsonValue::Type::Number) {
        utf16.column = static_cast<std::size_t>(character->second.number_value);
    }
    return core.position_for_utf16(utf16);
}

DiagnosticSeverity diagnostic_severity_from_lsp(const JsonValue &value) {
    if (value.type != JsonValue::Type::Number) {
        return DiagnosticSeverity::Warning;
    }
    return static_cast<int>(value.number_value) == 1 ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
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
            if (line.rfind("Content-Length:", 0) == 0) {
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

LspService::LspService(EditorConfig config) : config_(std::move(config)) {}

LspService::~LspService() {
    stop();
}

std::string LspService::name() const {
    return "lsp";
}

void LspService::start() {
    if (running_ || !config_.lsp_command || config_.lsp_command->empty()) {
        return;
    }
#if defined(__unix__) || defined(__APPLE__)
    if (!spawn_process()) {
        queue_status("LSP start failed");
        return;
    }
    running_ = true;
    reader_thread_ = std::thread(&LspService::reader_loop, this);
    send_initialize();
#else
    queue_status("LSP transport unsupported on this platform");
#endif
}

void LspService::stop() {
    if (!running_) {
        return;
    }
    send_shutdown();
    send_exit();
    shutdown_process();
    running_ = false;
    initialized_ = false;
}

void LspService::handle_editor_event(const EditorEvent &event) {
    if (!running_) {
        return;
    }
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

std::vector<ServiceEvent> LspService::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceEvent> events = std::move(pending_events_);
    pending_events_.clear();
    return events;
}

std::optional<int> LspService::poll_interval_ms() const {
    return 50;
}

void LspService::queue_event(ServiceEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.push_back(std::move(event));
}

void LspService::queue_status(const std::string &message) {
    EditorCommand command;
    command.type = EditorCommandType::SetStatusMessage;
    command.message = message;
    queue_event({ServiceEventType::Notification, name(), "status", command, std::nullopt, 0, std::nullopt, U""});
}

#if defined(__unix__) || defined(__APPLE__)
bool LspService::spawn_process() {
    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        return false;
    }

    int pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        execl("/bin/sh", "sh", "-lc", config_.lsp_command->c_str(), nullptr);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    child_pid_ = pid;
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    return true;
}

void LspService::shutdown_process() {
    if (stdin_fd_ >= 0) {
        close(stdin_fd_);
        stdin_fd_ = -1;
    }
    if (stdout_fd_ >= 0) {
        close(stdout_fd_);
        stdout_fd_ = -1;
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        waitpid(child_pid_, nullptr, 0);
        child_pid_ = -1;
    }
}

void LspService::reader_loop() {
    char chunk[4096];
    while (stdout_fd_ >= 0) {
        ssize_t read_count = read(stdout_fd_, chunk, sizeof(chunk));
        if (read_count <= 0) {
            break;
        }
        read_buffer_.append(chunk, static_cast<std::size_t>(read_count));
        for (const std::string &payload : extract_lsp_messages(read_buffer_)) {
            handle_message(payload);
        }
    }
}

bool LspService::write_payload(const std::string &payload) {
    if (stdin_fd_ < 0) {
        return false;
    }
    std::string framed = encode_lsp_message(payload);
    const char *data = framed.data();
    std::size_t remaining = framed.size();
    while (remaining > 0) {
        ssize_t wrote = write(stdin_fd_, data, remaining);
        if (wrote <= 0) {
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

void LspService::send_initialize() {
    initialize_request_id_ = next_request_id_++;
    std::string root = std::filesystem::current_path().string();
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"id\":" << initialize_request_id_ << ","
            << "\"method\":\"initialize\","
            << "\"params\":{"
            << "\"processId\":null,"
            << "\"rootUri\":" << json_string("file://" + root) << ","
            << "\"capabilities\":{},"
            << "\"clientInfo\":{\"name\":\"medit\"}"
            << "}"
            << "}";
    write_payload(payload.str());
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
    std::string language_id = config_.lsp_language_id.value_or("cpp");
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"method\":\"textDocument/didOpen\","
            << "\"params\":{\"textDocument\":{"
            << "\"uri\":" << json_string(event.document_uri) << ","
            << "\"languageId\":" << json_string(language_id) << ","
            << "\"version\":" << event.document_version << ","
            << "\"text\":" << json_string(u32_to_utf8(event.text))
            << "}}}";
    write_payload(payload.str());
}

void LspService::send_did_change(const EditorEvent &event) {
    if (!initialized_) {
        return;
    }
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"method\":\"textDocument/didChange\","
            << "\"params\":{"
            << "\"textDocument\":{\"uri\":" << json_string(event.document_uri) << ",\"version\":" << event.document_version << "},"
            << "\"contentChanges\":[{\"text\":" << json_string(u32_to_utf8(event.text)) << "}]"
            << "}}";
    write_payload(payload.str());
}

void LspService::send_did_save(const EditorEvent &event) {
    if (!initialized_) {
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
    if (!initialized_) {
        return;
    }
    std::ostringstream payload;
    payload << "{"
            << "\"jsonrpc\":\"2.0\","
            << "\"method\":\"textDocument/didClose\","
            << "\"params\":{\"textDocument\":{\"uri\":" << json_string(event.document_uri) << "}}"
            << "}";
    write_payload(payload.str());
}

void LspService::handle_message(const std::string &payload) {
    JsonValue root = parse_json(payload);
    if (root.type != JsonValue::Type::Object) {
        return;
    }

    auto id = root.object_value.find("id");
    if (id != root.object_value.end() && id->second.type == JsonValue::Type::Number &&
        static_cast<int>(id->second.number_value) == initialize_request_id_) {
        initialized_ = true;
        send_initialized();
        queue_status("LSP initialized");
        return;
    }

    auto method = root.object_value.find("method");
    if (method == root.object_value.end() || method->second.type != JsonValue::Type::String) {
        return;
    }

    if (method->second.string_value == "window/logMessage" || method->second.string_value == "window/showMessage") {
        auto params = root.object_value.find("params");
        if (params != root.object_value.end() && params->second.type == JsonValue::Type::Object) {
            auto message = params->second.object_value.find("message");
            if (message != params->second.object_value.end() && message->second.type == JsonValue::Type::String) {
                queue_status("LSP: " + message->second.string_value);
            }
        }
        return;
    }

    if (method->second.string_value != "textDocument/publishDiagnostics") {
        return;
    }

    auto params = root.object_value.find("params");
    if (params == root.object_value.end() || params->second.type != JsonValue::Type::Object) {
        return;
    }

    auto uri = params->second.object_value.find("uri");
    auto diagnostics = params->second.object_value.find("diagnostics");
    if (uri == params->second.object_value.end() || diagnostics == params->second.object_value.end() ||
        uri->second.type != JsonValue::Type::String || diagnostics->second.type != JsonValue::Type::Array) {
        return;
    }

    std::vector<Diagnostic> parsed_diagnostics;
    EditorCore conversion_core;
    std::string file_path = file_path_from_uri(uri->second.string_value);
    if (!file_path.empty()) {
        conversion_core.load_file(file_path);
    }
    for (const JsonValue &diagnostic_value : diagnostics->second.array_value) {
        if (diagnostic_value.type != JsonValue::Type::Object) {
            continue;
        }
        auto range = diagnostic_value.object_value.find("range");
        auto message = diagnostic_value.object_value.find("message");
        if (range == diagnostic_value.object_value.end() || message == diagnostic_value.object_value.end() ||
            range->second.type != JsonValue::Type::Object || message->second.type != JsonValue::Type::String) {
            continue;
        }
        auto start = range->second.object_value.find("start");
        auto end = range->second.object_value.find("end");
        if (start == range->second.object_value.end() || end == range->second.object_value.end() ||
            start->second.type != JsonValue::Type::Object || end->second.type != JsonValue::Type::Object) {
            continue;
        }
        Diagnostic parsed;
        parsed.range = {position_from_lsp(start->second, conversion_core), position_from_lsp(end->second, conversion_core)};
        auto severity = diagnostic_value.object_value.find("severity");
        if (severity != diagnostic_value.object_value.end()) {
            parsed.severity = diagnostic_severity_from_lsp(severity->second);
        }
        auto source = diagnostic_value.object_value.find("source");
        if (source != diagnostic_value.object_value.end() && source->second.type == JsonValue::Type::String) {
            parsed.source = source->second.string_value;
        }
        parsed.message = utf8_to_u32(message->second.string_value);
        parsed_diagnostics.push_back(std::move(parsed));
    }

    EditorCommand command;
    command.type = EditorCommandType::SetDiagnostics;
    command.document_uri = uri->second.string_value;
    command.diagnostics = std::move(parsed_diagnostics);
    queue_event({ServiceEventType::Notification, name(), "publishDiagnostics", command, uri->second.string_value, 0, std::nullopt, U""});
}
