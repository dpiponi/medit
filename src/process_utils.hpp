#pragma once

#include <optional>
#include <string>

std::optional<std::string> first_command_word(const std::string &command);
std::optional<std::string> missing_executable_in_command(const std::string &command);
std::optional<std::string> missing_executable_in_pipeline(const std::string &pipeline);
