#pragma once

#include "config.hpp"
#include "services.hpp"

#include <atomic>
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
    std::mutex mutex_;
    std::vector<ServiceEvent> pending_events_;
    std::vector<EditorEvent> pending_editor_events_;
    std::set<std::string> open_documents_;
    std::map<int, ServiceRequest> pending_requests_;
    std::string read_buffer_;
    std::string stderr_buffer_;

    void queue_event(ServiceEvent event);
    void queue_status(const std::string &message);
    void queue_stderr_line(const std::string &line);
    bool matches_document(const std::string &document_uri) const;
    void ensure_initialized_for_event(const EditorEvent &event);
    void send_editor_event(const EditorEvent &event);
    void flush_pending_editor_events();
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
    void handle_message(const std::string &payload);
};
