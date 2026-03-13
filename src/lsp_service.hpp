#pragma once

#include "config.hpp"
#include "services.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

std::string encode_lsp_message(const std::string &payload);
std::vector<std::string> extract_lsp_messages(std::string &buffer);

class LspService : public EditorService {
  public:
    explicit LspService(LspServerConfig config);
    ~LspService() override;

    std::string name() const override;
    void start() override;
    void stop() override;
    void handle_editor_event(const EditorEvent &event) override;
    void handle_request(const ServiceRequest &request) override;
    std::vector<ServiceEvent> poll() override;
    std::optional<int> poll_interval_ms() const override;
    std::string status_summary() const override;

  private:
    LspServerConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> stopping_{false};
    int child_pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    int next_request_id_ = 1;
    int initialize_request_id_ = -1;
    std::optional<std::filesystem::path> workspace_root_;
    std::thread reader_thread_;
    std::thread stderr_thread_;
    mutable std::mutex mutex_;
    std::vector<ServiceEvent> pending_events_;
    std::vector<EditorEvent> pending_editor_events_;
    std::map<std::string, EditorEvent> pending_document_changes_;
    std::map<std::string, std::chrono::steady_clock::time_point> pending_change_times_;
    std::map<std::string, std::u32string> pending_document_texts_;
    std::map<std::string, std::u32string> document_texts_;
    std::set<std::string> open_documents_;
    std::map<int, ServiceRequest> pending_requests_;
    std::string read_buffer_;
    std::string stderr_buffer_;
    std::string last_status_message_;
    std::string last_stderr_line_;

    void queue_event(ServiceEvent event);
    void queue_status(const std::string &message);
    void queue_stderr_line(const std::string &line);
    bool matches_document(const std::string &document_uri) const;
    void ensure_initialized_for_event(const EditorEvent &event);
    void send_editor_event(const EditorEvent &event);
    void flush_pending_editor_events();
    void flush_pending_document_changes(bool force = false, const std::optional<std::string> &document_uri = std::nullopt);
    bool spawn_process();
    void shutdown_process();
    void reader_loop();
    void stderr_loop();
    bool write_payload(const std::string &payload);
    void send_initialize(const std::filesystem::path &workspace_root);
    void send_initialized();
    void send_shutdown();
    void send_exit();
    void send_did_open(const EditorEvent &event);
    void send_did_change(const EditorEvent &event);
    void send_did_save(const EditorEvent &event);
    void send_did_close(const EditorEvent &event);
    void send_definition_request(const ServiceRequest &request);
    void send_hover_request(const ServiceRequest &request);
    void send_completion_request(const ServiceRequest &request);
    void send_selection_range_request(const ServiceRequest &request);
    void handle_message(const std::string &payload);
};
