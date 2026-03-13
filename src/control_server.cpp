#include "control_server.hpp"

#if defined(__unix__) || defined(__APPLE__)

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cstring>

namespace {

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

}

EditorControlServer::~EditorControlServer() {
    stop();
}

bool EditorControlServer::start(const std::filesystem::path &socket_path, std::string &error_message) {
    stop();

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error_message = "could not create control socket";
        return false;
    }

    std::filesystem::create_directories(socket_path.parent_path());
    std::error_code remove_error;
    std::filesystem::remove(socket_path, remove_error);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::string socket_string = socket_path.string();
    if (socket_string.size() >= sizeof(address.sun_path)) {
        close(fd);
        error_message = "control socket path too long";
        return false;
    }
    std::strncpy(address.sun_path, socket_string.c_str(), sizeof(address.sun_path) - 1);

    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        error_message = "could not bind control socket";
        return false;
    }
    if (listen(fd, 8) != 0) {
        close(fd);
        error_message = "could not listen on control socket";
        return false;
    }

    set_nonblocking(fd);
    listen_fd_ = fd;
    socket_path_ = socket_path;
    return true;
}

void EditorControlServer::stop() {
    for (std::size_t index = clients_.size(); index > 0; --index) {
        close_client(index - 1);
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (!socket_path_.empty()) {
        std::error_code error;
        std::filesystem::remove(socket_path_, error);
        socket_path_.clear();
    }
}

bool EditorControlServer::running() const {
    return listen_fd_ >= 0;
}

const std::filesystem::path &EditorControlServer::socket_path() const {
    return socket_path_;
}

void EditorControlServer::close_client(std::size_t index) {
    if (index >= clients_.size()) {
        return;
    }
    if (clients_[index].fd >= 0) {
        close(clients_[index].fd);
    }
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
}

void EditorControlServer::poll(const std::function<std::string(std::string_view)> &handler) {
    if (listen_fd_ < 0) {
        return;
    }

    while (true) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return;
        }
        set_nonblocking(client_fd);
        clients_.push_back({client_fd, ""});
    }

    char buffer[4096];
    for (std::size_t index = 0; index < clients_.size();) {
        ClientConnection &client = clients_[index];
        bool remove_client = false;
        while (true) {
            ssize_t read_count = read(client.fd, buffer, sizeof(buffer));
            if (read_count < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                remove_client = true;
                break;
            }
            if (read_count == 0) {
                remove_client = true;
                break;
            }
            client.input.append(buffer, static_cast<std::size_t>(read_count));
        }

        if (!remove_client) {
            std::size_t line_end = client.input.find('\n');
            if (line_end != std::string::npos) {
                std::string request = client.input.substr(0, line_end);
                if (!request.empty() && request.back() == '\r') {
                    request.pop_back();
                }
                std::string response = handler(request);
                response.push_back('\n');
                const char *data = response.data();
                std::size_t remaining = response.size();
                while (remaining > 0) {
                    ssize_t wrote = write(client.fd, data, remaining);
                    if (wrote < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            continue;
                        }
                        break;
                    }
                    data += wrote;
                    remaining -= static_cast<std::size_t>(wrote);
                }
                remove_client = true;
            }
        }

        if (remove_client) {
            close_client(index);
        } else {
            ++index;
        }
    }
}

#else

EditorControlServer::~EditorControlServer() = default;

bool EditorControlServer::start(const std::filesystem::path &, std::string &error_message) {
    error_message = "control socket unsupported on this platform";
    return false;
}

void EditorControlServer::stop() {}

bool EditorControlServer::running() const {
    return false;
}

const std::filesystem::path &EditorControlServer::socket_path() const {
    static const std::filesystem::path empty_path;
    return empty_path;
}

void EditorControlServer::poll(const std::function<std::string(std::string_view)> &) {}

#endif
