// Stub implementations for Windows where Unix features are unavailable
// This file is only compiled on Windows

#ifdef _WIN32

#include "lsp_service.hpp"
#include "control_server.hpp"
#include "clipboard.hpp"

#include <vector>
#include <string>
#include <optional>

// ============================================================================
// LSP Service Stubs (disabled on Windows for now)
// ============================================================================

LspService::LspService(LspServerConfig) {
    // Stub - LSP not available on Windows yet
}

LspService::~LspService() = default;

std::string LspService::name() const {
    return "lsp-stub";
}

void LspService::start() {
    // No-op
}

void LspService::stop() {
    // No-op
}

void LspService::handle_editor_event(const EditorEvent &) {
    // No-op
}

void LspService::handle_request(const ServiceRequest &) {
    // No-op
}

ServiceEvents LspService::poll() {
    return {};
}

std::optional<int> LspService::poll_interval_ms() const {
    return std::nullopt;
}

std::string LspService::status_summary() const {
    return "LSP not available on Windows";
}

// ============================================================================
// Control Server Stubs (disabled on Windows for now)
// ============================================================================

EditorControlServer::~EditorControlServer() = default;

bool EditorControlServer::start(const std::filesystem::path &, std::string &error_message) {
    error_message = "Control server not available on Windows";
    return false;
}

void EditorControlServer::stop() {
    // No-op
}

bool EditorControlServer::running() const {
    return false;
}

const std::filesystem::path &EditorControlServer::socket_path() const {
    static std::filesystem::path empty;
    return empty;
}

void EditorControlServer::poll(const std::function<std::string(std::string_view)> &) {
    // No-op
}

// ============================================================================
// Clipboard Implementation (minimal for now)
// ============================================================================

std::string read_system_clipboard() {
    // TODO: Implement Windows clipboard using GetClipboardData
    return "";
}

void write_system_clipboard(const std::string &) {
    // TODO: Implement Windows clipboard using SetClipboardData
}

std::string read_clipboard_file(const std::filesystem::path &) {
    return "";
}

void write_clipboard_file(const std::filesystem::path &, const std::string &) {
    // No-op
}

std::string read_clipboard_osc52() {
    return "";
}

void write_clipboard_osc52(const std::string &) {
    // No-op
}

ClipboardProvider::ClipboardProvider(ClipboardConfig config) : config_(std::move(config)) {
}

const ClipboardConfig &ClipboardProvider::config() const {
    return config_;
}

ClipboardSnapshot ClipboardProvider::read(const ClipboardSnapshot &fallback) const {
    // Minimal implementation - just return fallback
    return fallback;
}

bool ClipboardProvider::write(const ClipboardSnapshot &) const {
    // Minimal implementation - clipboard not functional yet
    return false;
}

ClipboardConfig default_clipboard_config() {
    ClipboardConfig config;
    config.mode = ClipboardMode::Internal;
    config.osc52 = false;
    return config;
}

// ============================================================================
// Process Utils Stubs
// ============================================================================

std::optional<std::string> missing_executable(const std::string &) {
    return std::nullopt;
}

std::optional<std::string> missing_executable_in_pipeline(const std::string &) {
    return std::nullopt;
}

bool executable_exists(std::string_view) {
    return false;
}

std::optional<std::string> first_command_word(std::string_view) {
    return std::nullopt;
}

std::optional<std::string> missing_executable_in_command(std::string_view) {
    return std::nullopt;
}

std::optional<std::string> first_available_executable(const std::vector<std::string> &executables) {
    // Simple stub - just return the first one if any exist
    if (executables.empty()) {
        return std::nullopt;
    }
    return executables[0];
}

std::vector<std::string> split_shell_pipeline(std::string_view) {
    return {};
}

// ============================================================================
// Services Stubs
// ============================================================================

std::unique_ptr<EditorService> create_lsp_service(LspServerConfig) {
    return nullptr;
}

#endif // _WIN32
