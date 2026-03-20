#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

using JsonValue = nlohmann::json;

JsonValue parse_json(const std::string &source);
std::string read_text_file(const std::filesystem::path &path);
