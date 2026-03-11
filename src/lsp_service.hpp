#pragma once

#include "config.hpp"
#include "services.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

std::string encode_lsp_message(const std::string &payload);
std::vector<std::string> extract_lsp_messages(std::string &buffer);

class LspService : public EditorService {
  public:
    explicit LspService(EditorConfig config);
    ~LspService() override;

    std::string name() const override;
    void start() override;
    void stop() override;
    void handle_editor_event(const EditorEvent &event) override;
    std::vector<ServiceEvent> poll() override;
    std::optional<int> poll_interval_ms() const override;

  private:
    EditorConfig config_;
    bool running_ = false;
    bool initialized_ = false;
    int child_pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int next_request_id_ = 1;
    int initialize_request_id_ = -1;
    std::thread reader_thread_;
    std::mutex mutex_;
    std::vector<ServiceEvent> pending_events_;
    std::string read_buffer_;

    void queue_event(ServiceEvent event);
    void queue_status(const std::string &message);
    bool spawn_process();
    void shutdown_process();
    void reader_loop();
    bool write_payload(const std::string &payload);
    void send_initialize();
    void send_initialized();
    void send_shutdown();
    void send_exit();
    void send_did_open(const EditorEvent &event);
    void send_did_change(const EditorEvent &event);
    void send_did_save(const EditorEvent &event);
    void send_did_close(const EditorEvent &event);
    void handle_message(const std::string &payload);
};
