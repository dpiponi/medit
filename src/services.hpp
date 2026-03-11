#pragma once

#include "editor_core.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class ServiceEventType {
    ServiceStarted,
    ServiceStopped,
    Notification,
};

struct ServiceEvent {
    ServiceEventType type = ServiceEventType::Notification;
    std::string service_name;
    std::string topic;
    std::optional<std::string> document_uri;
    std::size_t document_version = 0;
    std::optional<Range> range;
    std::u32string text;
};

class EditorService {
  public:
    virtual ~EditorService() = default;

    virtual std::string name() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void handle_editor_event(const EditorEvent &event) = 0;
    virtual std::vector<ServiceEvent> poll() = 0;
};

class EditorRuntime {
  public:
    void add_service(std::unique_ptr<EditorService> service);
    std::size_t service_count() const;
    bool started() const;

    void start_services();
    void stop_services();
    void dispatch_editor_events(EditorCore &core);
    void poll_services();

    const std::vector<ServiceEvent> &pending_service_events() const;
    std::vector<ServiceEvent> take_service_events();

  private:
    std::vector<std::unique_ptr<EditorService>> services_;
    std::vector<ServiceEvent> pending_service_events_;
    bool started_ = false;

    void append_events(std::vector<ServiceEvent> events);
};
