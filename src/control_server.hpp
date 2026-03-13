#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class EditorControlServer {
  public:
    ~EditorControlServer();

    bool start(const std::filesystem::path &socket_path, std::string &error_message);
    void stop();
    bool running() const;
    const std::filesystem::path &socket_path() const;
    void poll(const std::function<std::string(std::string_view)> &handler);

  private:
    struct ClientConnection {
        int fd = -1;
        std::string input;
    };

    int listen_fd_ = -1;
    std::filesystem::path socket_path_;
    std::vector<ClientConnection> clients_;

    void close_client(std::size_t index);
};
