#pragma once

#include <optional>
#include <string>
#include <vector>

std::optional<std::string> first_command_word(const std::string &command);
bool executable_exists(const std::string &executable);
std::optional<std::string> first_available_executable(const std::vector<std::string> &executables);
std::optional<std::string> missing_executable_in_command(const std::string &command);
std::optional<std::string> missing_executable_in_pipeline(const std::string &pipeline);
