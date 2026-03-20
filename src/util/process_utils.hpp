#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

std::optional<std::string> first_command_word(std::string_view command);
bool executable_exists(std::string_view executable);
std::optional<std::string> first_available_executable(const std::vector<std::string> &executables);
std::optional<std::string> missing_executable_in_command(std::string_view command);
std::optional<std::string> missing_executable_in_pipeline(std::string_view pipeline);
