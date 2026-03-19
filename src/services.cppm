module;

#include <memory>
#include <optional>
#include <string>
#include <vector>

export module services;

export import editor_commands;
export import editor_core;

export enum class ServiceEventType {
    ServiceStarted,
    ServiceStopped,
    Notification,
};

export enum class ServiceRequestType {
    GoToDefinition,
    Hover,
    WarmHover,
    Completion,
    SelectionRange,
};

export struct ServiceRequest {
    ServiceRequestType type = ServiceRequestType::GoToDefinition;
    std::string document_uri;
    Utf16Position utf16_position;
    std::size_t document_version = 0;
    std::string completion_prefix;
    std::optional<Range> completion_range;
};

export struct ServiceEvent {
    ServiceEventType type = ServiceEventType::Notification;
    std::string service_name;
    std::string topic;
    std::optional<EditorCommand> command;
    std::optional<std::string> document_uri;
    std::size_t document_version = 0;
    std::optional<Range> range;
    std::u32string text;
};

export using ServiceEvents = std::vector<ServiceEvent>;

export class EditorService {
  public:
    virtual ~EditorService() = default;

    virtual std::string name() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void handle_editor_event(const EditorEvent &event) = 0;
    virtual void handle_request(const ServiceRequest &request);
    virtual std::vector<ServiceEvent> poll() = 0;
    virtual std::optional<int> poll_interval_ms() const;
    virtual std::string status_summary() const;
};

export class EditorRuntime {
  public:
    void add_service(std::unique_ptr<EditorService> service);
    std::size_t service_count() const;
    bool started() const;

    void start_services();
    void stop_services();
    void clear_services();
    void process(EditorCore &core);
    void dispatch_editor_event(const EditorEvent &event);
    void dispatch_editor_events(EditorCore &core);
    void dispatch_service_request(const ServiceRequest &request);
    void poll_services();
    std::optional<int> idle_wait_timeout_ms() const;
    std::string status_summary() const;

    const std::vector<ServiceEvent> &pending_service_events() const;
    std::vector<ServiceEvent> take_service_events();

  private:
    std::vector<std::unique_ptr<EditorService>> services_;
    std::vector<ServiceEvent> pending_service_events_;
    bool started_ = false;

    void append_events(std::vector<ServiceEvent> events);
};
