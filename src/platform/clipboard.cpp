module;

#include "logger.hpp"
#include "process_utils.hpp"
#include "text_encoding_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

module clipboard;

namespace {

struct NativeClipboardCommands {
    std::optional<std::string> read_command;
    std::optional<std::string> write_command;
    std::string name;
};

bool running_under_wsl() {
#if defined(__linux__)
    if (std::getenv("WSL_INTEROP") != nullptr || std::getenv("WSL_DISTRO_NAME") != nullptr) {
        return true;
    }
    std::ifstream input("/proc/sys/kernel/osrelease");
    if (!input) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string text = buffer.str();
    for (char &ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text.contains("microsoft") || text.contains("wsl");
#else
    return false;
#endif
}

std::string shell_single_quote(std::string_view text) {
    std::string quoted = "'";
    for (char ch : text) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string base64_encode(std::string_view input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((input.size() + 2) / 3) * 4);

    std::size_t index = 0;
    while (index + 3 <= input.size()) {
        unsigned value =
            (static_cast<unsigned char>(input[index]) << 16) |
            (static_cast<unsigned char>(input[index + 1]) << 8) |
            static_cast<unsigned char>(input[index + 2]);
        encoded.push_back(kAlphabet[(value >> 18) & 0x3f]);
        encoded.push_back(kAlphabet[(value >> 12) & 0x3f]);
        encoded.push_back(kAlphabet[(value >> 6) & 0x3f]);
        encoded.push_back(kAlphabet[value & 0x3f]);
        index += 3;
    }

    std::size_t remaining = input.size() - index;
    if (remaining == 1) {
        unsigned value = static_cast<unsigned char>(input[index]) << 16;
        encoded.push_back(kAlphabet[(value >> 18) & 0x3f]);
        encoded.push_back(kAlphabet[(value >> 12) & 0x3f]);
        encoded.push_back('=');
        encoded.push_back('=');
    } else if (remaining == 2) {
        unsigned value =
            (static_cast<unsigned char>(input[index]) << 16) |
            (static_cast<unsigned char>(input[index + 1]) << 8);
        encoded.push_back(kAlphabet[(value >> 18) & 0x3f]);
        encoded.push_back(kAlphabet[(value >> 12) & 0x3f]);
        encoded.push_back(kAlphabet[(value >> 6) & 0x3f]);
        encoded.push_back('=');
    }

    return encoded;
}

std::filesystem::path default_shared_clipboard_path() {
    std::string suffix = "medit-clipboard.json";
#if defined(__unix__) || defined(__APPLE__)
    suffix = "medit-" + std::to_string(static_cast<long long>(getuid())) + "-clipboard.json";
#endif
    return std::filesystem::temp_directory_path() / suffix;
}

ClipboardSnapshot parse_shared_clipboard_json(const std::string &source) {
    ClipboardSnapshot snapshot;
    std::size_t mode_key = source.find("\"mode\"");
    std::size_t text_key = source.find("\"text\"");
    if (mode_key == std::string::npos || text_key == std::string::npos) {
        throw std::runtime_error("missing clipboard fields");
    }

    std::size_t mode_value_start = source.find('"', source.find(':', mode_key));
    std::size_t mode_value_end = source.find('"', mode_value_start + 1);
    std::string mode = source.substr(mode_value_start + 1, mode_value_end - mode_value_start - 1);
    snapshot.mode = mode == "line" ? SelectionMode::Line : SelectionMode::Character;

    std::size_t text_value_start = source.find('"', source.find(':', text_key));
    std::size_t cursor = text_value_start + 1;
    std::string text;
    bool escaped = false;
    while (cursor < source.size()) {
        char ch = source[cursor++];
        if (escaped) {
            switch (ch) {
                case 'n':
                    text.push_back('\n');
                    break;
                case 'r':
                    text.push_back('\r');
                    break;
                case 't':
                    text.push_back('\t');
                    break;
                case '\\':
                case '"':
                    text.push_back(ch);
                    break;
                default:
                    text.push_back(ch);
                    break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            break;
        }
        text.push_back(ch);
    }
    snapshot.text = utf8_to_u32(text);
    return snapshot;
}

std::string shared_clipboard_json(const ClipboardSnapshot &snapshot) {
    std::string escaped;
    for (char ch : u32_to_utf8(snapshot.text)) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }

    return std::string("{\"mode\":\"") +
        (snapshot.mode == SelectionMode::Line ? "line" : "character") +
        "\",\"text\":\"" + escaped + "\"}";
}

bool with_locked_file(
    const std::filesystem::path &path,
    bool create_parent_directories,
    const std::function<bool()> &fn) {
#if defined(__unix__) || defined(__APPLE__)
    if (create_parent_directories) {
        std::filesystem::create_directories(path.parent_path());
    }
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        return false;
    }
    bool locked = flock(fd, LOCK_EX) == 0;
    bool result = false;
    if (locked) {
        result = fn();
        flock(fd, LOCK_UN);
    }
    close(fd);
    return locked && result;
#else
    return fn();
#endif
}

std::optional<ClipboardSnapshot> read_shared_file_clipboard(const std::filesystem::path &path) {
    const std::filesystem::path lock_path = path.string() + ".lock";
    std::optional<ClipboardSnapshot> snapshot;
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }
    bool ok = with_locked_file(lock_path, false, [&]() {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return true;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        try {
            snapshot = parse_shared_clipboard_json(buffer.str());
        } catch (const std::exception &error) {
            log_debug("clipboard shared-file parse failed path=" + path.string() + " error=" + error.what());
        }
        return true;
    });
    if (!ok) {
        log_debug("clipboard shared-file lock failed path=" + path.string());
    }
    return snapshot;
}

bool write_shared_file_clipboard(const std::filesystem::path &path, const ClipboardSnapshot &snapshot) {
    const std::filesystem::path lock_path = path.string() + ".lock";
    return with_locked_file(lock_path, true, [&]() {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::filesystem::path temp_path = path;
        temp_path += ".tmp";
        {
            std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
            if (!output) {
                return false;
            }
            output << shared_clipboard_json(snapshot);
        }
        std::filesystem::rename(temp_path, path, error);
        if (error) {
            std::filesystem::remove(temp_path);
            return false;
        }
        return true;
    });
}

std::optional<std::string> run_capture_command(std::string_view command) {
#if defined(__unix__) || defined(__APPLE__)
    std::string shell_command = "sh -c " + shell_single_quote(command);
    FILE *pipe = popen(shell_command.c_str(), "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    int status = pclose(pipe);
    if (status != 0) {
        return std::nullopt;
    }
    return output;
#else
    (void)command;
    return std::nullopt;
#endif
}

bool run_write_command(std::string_view command, const std::string &input_text) {
#if defined(__unix__) || defined(__APPLE__)
    char input_path[] = "/tmp/medit-clipboard-in-XXXXXX";
    int input_fd = mkstemp(input_path);
    if (input_fd < 0) {
        return false;
    }
    close(input_fd);
    {
        std::ofstream output(input_path, std::ios::binary);
        output << input_text;
    }
    std::string shell_command =
        "sh -c " + shell_single_quote(std::string(command) + " < " + shell_single_quote(input_path));
    int status = std::system(shell_command.c_str());
    std::filesystem::remove(input_path);
    return status == 0;
#else
    (void)command;
    (void)input_text;
    return false;
#endif
}

NativeClipboardCommands native_clipboard_commands() {
    NativeClipboardCommands commands;
#if defined(__APPLE__)
    if (executable_exists("pbcopy") && executable_exists("pbpaste")) {
        commands.write_command = "pbcopy";
        commands.read_command = "pbpaste";
    commands.name = "pbcopy/pbpaste";
    }
#else
    if (running_under_wsl() && executable_exists("clip.exe")) {
        commands.write_command = "clip.exe";
        if (executable_exists("powershell.exe")) {
            commands.read_command =
                "powershell.exe -NoProfile -Command \"[Console]::Out.Write((Get-Clipboard -Raw))\"";
            commands.name = "clip.exe/powershell.exe";
        } else if (executable_exists("pwsh.exe")) {
            commands.read_command =
                "pwsh.exe -NoProfile -Command \"[Console]::Out.Write((Get-Clipboard -Raw))\"";
            commands.name = "clip.exe/pwsh.exe";
        } else {
            commands.name = "clip.exe";
        }
    } else {
        const bool wayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
        const bool x11 = std::getenv("DISPLAY") != nullptr;
        if (wayland && executable_exists("wl-copy") && executable_exists("wl-paste")) {
            commands.write_command = "wl-copy";
            commands.read_command = "wl-paste --no-newline";
            commands.name = "wl-copy/wl-paste";
        } else if (x11 && executable_exists("xclip")) {
            commands.write_command = "xclip -selection clipboard";
            commands.read_command = "xclip -selection clipboard -o";
            commands.name = "xclip";
        } else if (x11 && executable_exists("xsel")) {
            commands.write_command = "xsel --clipboard --input";
            commands.read_command = "xsel --clipboard --output";
            commands.name = "xsel";
        }
    }
#endif
    return commands;
}

bool write_native_clipboard(const ClipboardSnapshot &snapshot) {
    NativeClipboardCommands commands = native_clipboard_commands();
    if (!commands.write_command) {
        return false;
    }
    bool ok = run_write_command(*commands.write_command, u32_to_utf8(snapshot.text));
    log_debug(
        "clipboard native write backend=" + commands.name + " status=" + (ok ? "ok" : "failed"));
    return ok;
}

std::optional<ClipboardSnapshot> read_native_clipboard(SelectionMode fallback_mode) {
    NativeClipboardCommands commands = native_clipboard_commands();
    if (!commands.read_command) {
        return std::nullopt;
    }
    std::optional<std::string> output = run_capture_command(*commands.read_command);
    if (!output) {
        log_debug("clipboard native read backend=" + commands.name + " status=failed");
        return std::nullopt;
    }
    while (!output->empty() && (output->back() == '\n' || output->back() == '\r')) {
        output->pop_back();
    }
    ClipboardSnapshot snapshot;
    snapshot.text = utf8_to_u32(*output);
    snapshot.mode = fallback_mode;
    log_debug("clipboard native read backend=" + commands.name + " status=ok");
    return snapshot;
}

bool osc52_supported() {
#if defined(__unix__) || defined(__APPLE__)
    return isatty(STDOUT_FILENO) != 0;
#else
    return false;
#endif
}

bool write_osc52_clipboard(const ClipboardSnapshot &snapshot) {
#if defined(__unix__) || defined(__APPLE__)
    if (!osc52_supported()) {
        return false;
    }
    std::string payload = base64_encode(u32_to_utf8(snapshot.text));
    std::string sequence = "\033]52;c;" + payload + "\a";
    ssize_t written = ::write(STDOUT_FILENO, sequence.c_str(), sequence.size());
    bool ok = written == static_cast<ssize_t>(sequence.size());
    log_debug("clipboard osc52 write status=" + std::string(ok ? "ok" : "failed"));
    return ok;
#else
    (void)snapshot;
    return false;
#endif
}

}  // namespace

ClipboardProvider::ClipboardProvider(ClipboardConfig config)
    : config_(std::move(config)) {
    if (config_.shared_file_path.empty()) {
        config_.shared_file_path = default_shared_clipboard_path();
    }
    log_debug(
        "clipboard configured mode=" +
        std::to_string(static_cast<int>(config_.mode)) +
        " file=" + config_.shared_file_path.string() +
        " osc52=" + (config_.osc52 ? "true" : "false"));
}

const ClipboardConfig &ClipboardProvider::config() const {
    return config_;
}

ClipboardSnapshot ClipboardProvider::read(const ClipboardSnapshot &fallback) const {
    if (config_.mode == ClipboardMode::Internal) {
        return fallback;
    }

    std::optional<ClipboardSnapshot> shared_snapshot;
    if (config_.mode == ClipboardMode::Auto || config_.mode == ClipboardMode::SharedFile) {
        shared_snapshot = read_shared_file_clipboard(config_.shared_file_path);
    }

    if (config_.mode == ClipboardMode::Auto || config_.mode == ClipboardMode::Native) {
        if (std::optional<ClipboardSnapshot> native = read_native_clipboard(fallback.mode)) {
            if (shared_snapshot && shared_snapshot->text == native->text) {
                native->mode = shared_snapshot->mode;
            }
            return *native;
        }
        if (config_.mode == ClipboardMode::Native) {
            return fallback;
        }
    }

    if (shared_snapshot) {
        return *shared_snapshot;
    }

    return fallback;
}

bool ClipboardProvider::write(const ClipboardSnapshot &snapshot) const {
    if (config_.mode == ClipboardMode::Internal) {
        return true;
    }

    bool wrote_anything = false;
    if (config_.mode == ClipboardMode::Auto || config_.mode == ClipboardMode::Native) {
        wrote_anything = write_native_clipboard(snapshot) || wrote_anything;
        if (config_.osc52 && config_.mode == ClipboardMode::Auto) {
            wrote_anything = write_osc52_clipboard(snapshot) || wrote_anything;
        }
        if (config_.mode == ClipboardMode::Native) {
            return wrote_anything;
        }
    }

    if (config_.mode == ClipboardMode::Auto || config_.mode == ClipboardMode::SharedFile) {
        bool shared_ok = write_shared_file_clipboard(config_.shared_file_path, snapshot);
        log_debug(
            "clipboard shared-file write path=" + config_.shared_file_path.string() +
            " status=" + (shared_ok ? "ok" : "failed"));
        wrote_anything = shared_ok || wrote_anything;
    }

    return wrote_anything;
}

ClipboardConfig default_clipboard_config() {
    ClipboardConfig config;
    config.mode = ClipboardMode::Auto;
    config.shared_file_path = default_shared_clipboard_path();
    config.osc52 = true;
    return config;
}
