#include "logger.hpp"

#include <chrono>
#include <format>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace {

std::mutex logger_mutex;
std::optional<std::filesystem::path> logger_path;

std::string timestamp_now() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t time = clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

}  // namespace

void configure_logger(const std::optional<std::filesystem::path> &path) {
    std::lock_guard<std::mutex> lock(logger_mutex);
    logger_path = path;
    if (!logger_path) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(logger_path->parent_path(), error);
    std::ofstream output(*logger_path, std::ios::app);
    if (output) {
        output << std::format("[{}] logger configured: {}\n", timestamp_now(), logger_path->string());
    }
}

void log_debug(const std::string &message) {
    std::lock_guard<std::mutex> lock(logger_mutex);
    if (!logger_path) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(logger_path->parent_path(), error);
    std::ofstream output(*logger_path, std::ios::app);
    if (!output) {
        return;
    }
    output << std::format("[{}] {}\n", timestamp_now(), message);
}
