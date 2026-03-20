#pragma once

#include <string>

// URI encoding/decoding utilities

std::string file_uri_for_path(const std::string &path);
std::string file_path_from_uri(const std::string &uri);
std::string normalize_document_uri(const std::string &uri);
std::string percent_encode_path(const std::string &path);
std::string percent_decode_path(const std::string &path);
