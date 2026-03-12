#include "services.hpp"

#include <algorithm>
#include <utility>

std::optional<int> EditorService::poll_interval_ms() const {
    return std::nullopt;
}

std::string EditorService::status_summary() const {
    return name();
}

void EditorService::handle_request(const ServiceRequest &request) {
    (void)request;
}

void EditorRuntime::add_service(std::unique_ptr<EditorService> service) {
    if (!service) {
        return;
    }

    if (started_) {
        service->start();
        pending_service_events_.push_back(
            {ServiceEventType::ServiceStarted, service->name(), "service_started", std::nullopt, std::nullopt, 0, std::nullopt, U""});
    }
    services_.push_back(std::move(service));
}

std::size_t EditorRuntime::service_count() const {
    return services_.size();
}

bool EditorRuntime::started() const {
    return started_;
}

void EditorRuntime::start_services() {
    if (started_) {
        return;
    }

    started_ = true;
    for (const std::unique_ptr<EditorService> &service : services_) {
        service->start();
        pending_service_events_.push_back(
            {ServiceEventType::ServiceStarted, service->name(), "service_started", std::nullopt, std::nullopt, 0, std::nullopt, U""});
    }
}

void EditorRuntime::stop_services() {
    if (!started_) {
        return;
    }

    for (const std::unique_ptr<EditorService> &service : services_) {
        service->stop();
        pending_service_events_.push_back(
            {ServiceEventType::ServiceStopped, service->name(), "service_stopped", std::nullopt, std::nullopt, 0, std::nullopt, U""});
    }
    started_ = false;
}

void EditorRuntime::clear_services() {
    if (started_) {
        stop_services();
    }
    services_.clear();
}

void EditorRuntime::process(EditorCore &core) {
    dispatch_editor_events(core);
    poll_services();
}

void EditorRuntime::dispatch_editor_event(const EditorEvent &event) {
    for (const std::unique_ptr<EditorService> &service : services_) {
        service->handle_editor_event(event);
    }
}

void EditorRuntime::dispatch_editor_events(EditorCore &core) {
    std::vector<EditorEvent> events = core.take_events();
    if (events.empty()) {
        return;
    }

    for (const EditorEvent &event : events) {
        dispatch_editor_event(event);
    }
}

void EditorRuntime::dispatch_service_request(const ServiceRequest &request) {
    for (const std::unique_ptr<EditorService> &service : services_) {
        service->handle_request(request);
    }
}

void EditorRuntime::poll_services() {
    for (const std::unique_ptr<EditorService> &service : services_) {
        append_events(service->poll());
    }
}

std::optional<int> EditorRuntime::idle_wait_timeout_ms() const {
    if (!started_) {
        return std::nullopt;
    }

    std::optional<int> timeout_ms;
    for (const std::unique_ptr<EditorService> &service : services_) {
        std::optional<int> service_timeout = service->poll_interval_ms();
        if (!service_timeout.has_value()) {
            continue;
        }

        int clamped_timeout = std::max(0, *service_timeout);
        if (!timeout_ms.has_value() || clamped_timeout < *timeout_ms) {
            timeout_ms = clamped_timeout;
        }
    }
    return timeout_ms;
}

std::string EditorRuntime::status_summary() const {
    if (services_.empty()) {
        return "No background services configured.";
    }

    std::string summary;
    for (std::size_t index = 0; index < services_.size(); ++index) {
        if (index > 0) {
            summary += "\n\n";
        }
        summary += services_[index]->status_summary();
    }
    return summary;
}

const std::vector<ServiceEvent> &EditorRuntime::pending_service_events() const {
    return pending_service_events_;
}

std::vector<ServiceEvent> EditorRuntime::take_service_events() {
    std::vector<ServiceEvent> events = std::move(pending_service_events_);
    pending_service_events_.clear();
    return events;
}

void EditorRuntime::append_events(std::vector<ServiceEvent> events) {
    pending_service_events_.insert(
        pending_service_events_.end(),
        std::make_move_iterator(events.begin()),
        std::make_move_iterator(events.end()));
}
