#include "uri_utils.hpp"

#include <filesystem>

namespace {

bool is_unreserved_uri_byte(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
        ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

}  // namespace

std::string percent_encode_path(const std::string &path) {
    static const char *kHex = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char ch : path) {
        if (is_unreserved_uri_byte(ch)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(ch >> 4) & 0x0F]);
        encoded.push_back(kHex[ch & 0x0F]);
    }
    return encoded;
}

std::string percent_decode_path(const std::string &path) {
    std::string decoded;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            int high = hex_value(path[i + 1]);
            int low = hex_value(path[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(path[i]);
    }
    return decoded;
}

std::string file_uri_for_path(const std::string &path) {
    std::filesystem::path absolute = std::filesystem::absolute(path);
    return "file://" + percent_encode_path(absolute.string());
}

std::string file_path_from_uri(const std::string &uri) {
    if (!uri.starts_with("file://")) {
        return "";
    }
    return percent_decode_path(uri.substr(7));
}

std::string normalize_document_uri(const std::string &uri) {
    if (!uri.starts_with("file://")) {
        return uri;
    }
    std::string path = file_path_from_uri(uri);
    if (path.empty()) {
        return uri;
    }
    return file_uri_for_path(path);
}
