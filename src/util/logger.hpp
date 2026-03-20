#pragma once

#include <filesystem>
#include <optional>
#include <string>

void configure_logger(const std::optional<std::filesystem::path> &path);
void log_debug(const std::string &message);
