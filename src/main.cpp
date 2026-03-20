#include "editor_app.hpp"
#include "config.hpp"
#include "keybindings.hpp"
#include "theme.hpp"
#include "services.hpp"
#include "lsp_service.hpp"
#include "logger.hpp"
#include "process_utils.hpp"
#include "string_utils.hpp"

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::string path_or_none(const std::optional<std::filesystem::path> &path) {
    return path ? path->string() : "(none)";
}

void append_health_line(std::ostringstream &output, const std::string &label, const std::string &value) {
    output << label << ": " << value << "\n";
}

std::string lsp_health_summary(const EditorConfig &config) {
    std::ostringstream output;
    output << "configured servers: " << config.lsp_servers.size() << "\n";
    for (const LspServerConfig &server : config.lsp_servers) {
        output << "\n[" << server.name << "]\n";
        output << "command: " << server.command << "\n";
        output << "language id: " << server.language_id << "\n";
        output << "patterns: ";
        for (std::size_t index = 0; index < server.patterns.size(); ++index) {
            if (index > 0) {
                output << ", ";
            }
            output << server.patterns[index];
        }
        output << "\n";

        LspService service(server);
        service.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        (void)service.poll();
        output << service.status_summary() << "\n";
        service.stop();
    }
    return output.str();
}

std::optional<std::string> resolve_ai_command(const EditorConfig &config) {
    if (config.ai_command && !config.ai_command->empty()) {
        return config.ai_command;
    }
    if (executable_exists("medit-ai")) {
        return std::string("medit-ai");
    }
    if (executable_exists("./tools/medit_ai.py")) {
        return std::string("./tools/medit_ai.py");
    }
    if (executable_exists("tools/medit_ai.py")) {
        return std::string("tools/medit_ai.py");
    }
    return std::nullopt;
}

std::string resolve_ai_provider(const EditorConfig &config) {
    if (config.ai_provider && !config.ai_provider->empty()) {
        return *config.ai_provider;
    }
    const char *llm_provider = std::getenv("LLM_PROVIDER");
    if (llm_provider != nullptr) {
        std::string provider = ascii_lowercase(llm_provider);
        if (provider == "openai" || provider == "mistral") {
            return provider;
        }
    }
    const char *openai_key = std::getenv("OPENAI_API_KEY");
    if (openai_key != nullptr && *openai_key != '\0') {
        return "openai";
    }
    const char *mistral_key = std::getenv("MISTRAL_API_KEY");
    if (mistral_key != nullptr && *mistral_key != '\0') {
        return "mistral";
    }
    return "openai";
}

std::string resolve_ai_model(const EditorConfig &config, std::string_view provider) {
    if (config.ai_model && !config.ai_model->empty()) {
        return *config.ai_model;
    }
    const char *llm_model = std::getenv("LLM_MODEL");
    if (llm_model != nullptr && *llm_model != '\0') {
        return llm_model;
    }
    if (provider == "mistral") {
        const char *mistral_model = std::getenv("MISTRAL_MODEL");
        if (mistral_model != nullptr && *mistral_model != '\0') {
            return mistral_model;
        }
        return "mistral-small-latest";
    }
    const char *openai_model = std::getenv("OPENAI_MODEL");
    if (openai_model != nullptr && *openai_model != '\0') {
        return openai_model;
    }
    return "gpt-5-nano";
}

std::string ai_api_key_status(std::string_view provider) {
    const char *env_name = provider == "mistral" ? "MISTRAL_API_KEY" : "OPENAI_API_KEY";
    const char *value = std::getenv(env_name);
    return value != nullptr && *value != '\0' ? std::string(env_name) + " set" : std::string(env_name) + " missing";
}

std::string ai_helper_status(const std::optional<std::string> &command) {
    if (!command) {
        return "missing";
    }
    if (std::optional<std::string> missing = missing_executable_in_command(*command)) {
        return "missing executable: " + *missing;
    }
    return "ok";
}

int run_health_check() {
    initialize_locale();

    EditorConfig config;
    KeyBindings keybindings;
    bool ok = true;

    std::optional<std::string> config_error;
    std::optional<std::string> keybindings_error;
    std::optional<std::string> theme_error;
    std::optional<std::string> lua_error;

    try {
        config = load_editor_config();
        configure_logger(config.log_path);
    } catch (const std::exception &error) {
        config_error = error.what();
        ok = false;
    }

    if (!config_error) {
        try {
            keybindings = load_keybindings(config);
        } catch (const std::exception &error) {
            keybindings_error = error.what();
            ok = false;
        }

        try {
            (void)load_theme(config);
        } catch (const std::exception &error) {
            theme_error = error.what();
            ok = false;
        }

        if (config.lua_path) {
            EditorState state;
            initialize_windows(state);
            state.config = config;
            std::string error_message;
            if (!state.lua.initialize(state, config.lua_path, error_message)) {
                lua_error = error_message;
                ok = false;
            }
        }
    }

    std::ostringstream output;
    output << "medit health\n\n";

    output << "Config files\n";
    append_health_line(output, "meditrc", config.source_path.empty() ? "(default/none)" : config.source_path);
    append_health_line(output, "keybindings", path_or_none(config.keybindings_path));
    append_health_line(output, "colors", path_or_none(config.colors_path));
    append_health_line(output, "lsp", path_or_none(config.lsp_path));
    append_health_line(output, "syntax", path_or_none(config.syntax_config_path));
    append_health_line(output, "lua", path_or_none(config.lua_path));
    append_health_line(output, "log", path_or_none(config.log_path));
    append_health_line(output, "control socket", path_or_none(config.control_socket_path));
    if (config_error) {
        append_health_line(output, "config error", *config_error);
    }
    if (keybindings_error) {
        append_health_line(output, "keybindings error", *keybindings_error);
    }
    if (theme_error) {
        append_health_line(output, "theme error", *theme_error);
    }
    if (lua_error) {
        append_health_line(output, "lua error", *lua_error);
    }

    if (!config_error) {
        output << "\nLSP\n";
        output << lsp_health_summary(config);

        output << "\nTree-sitter\n";
        output << tree_sitter_health_summary(config) << "\n";

        if (config.lua_path) {
            EditorState state;
            initialize_windows(state);
            state.config = config;
            std::string error_message;
            if (state.lua.initialize(state, config.lua_path, error_message)) {
                std::vector<std::pair<std::string, std::string>> lua_health = state.lua.run_health_checks(state);
                if (!lua_health.empty()) {
                    output << "\nLua plugins\n";
                    for (const auto &[name, status] : lua_health) {
                        append_health_line(output, name, status);
                    }
                }
            }
        }

        const std::string ai_provider = resolve_ai_provider(config);
        output << "\nAI\n";
        append_health_line(output, "command", resolve_ai_command(config).value_or("(none)"));
        append_health_line(output, "command status", ai_helper_status(resolve_ai_command(config)));
        append_health_line(output, "provider", ai_provider);
        append_health_line(output, "model", resolve_ai_model(config, ai_provider));
        append_health_line(output, "auth", ai_api_key_status(ai_provider));
    }

    std::fputs(output.str().c_str(), stdout);
    return ok ? 0 : 1;
}

}  // namespace

import theme;

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--health") {
        return run_health_check();
    }
    initialize_locale();
    EditorState state;
    std::optional<std::string> startup_picker_root;
    try {
        state.config = load_editor_config();
        configure_logger(state.config.log_path);
        log_debug("editor startup");
        state.session.configure_clipboard(state.config.clipboard);
        state.keybindings = load_keybindings(state.config);
    } catch (const std::exception &error) {
        state.keybindings = load_embedded_keybindings();
        state.set_status(std::string("Keybindings config error: ") + error.what());
    }
    try {
        state.theme = load_theme(state.config);
    } catch (const std::exception &error) {
        state.theme = load_embedded_theme();
        state.set_status(std::string("Theme config error: ") + error.what());
    }
    if (!suspend_supported()) {
        log_debug("suspend not supported - removing suspend keybinding");
        remove_action_bindings(state.keybindings, EditorAction::Suspend);
    } else {
        log_debug("suspend is supported - keeping suspend keybinding");
    }
    initialize_windows(state);
    if (state.config.lua_path) {
        std::string lua_error;
        if (!state.lua.initialize(state, state.config.lua_path, lua_error)) {
            state.set_status("Lua init failed: " + lua_error);
        }
    }
    if (argc > 1) {
        startup_picker_root = open_startup_files(state, argc, argv);
    } else if (!state.config.source_path.empty()) {
        state.set_status("Config: " + state.config.source_path);
    } else if (!state.keybindings.source_path.empty()) {
        state.set_status("Keybindings: " + state.keybindings.source_path);
    }

    try {
        setup_terminal(state.theme);
        if (startup_picker_root) {
            state.open_startup_file_picker(std::filesystem::path(*startup_picker_root));
        }
        if (state.config.control_socket_path) {
            std::string control_error;
            if (state.control_server.start(*state.config.control_socket_path, control_error)) {
                log_debug("control socket started path=" + state.config.control_socket_path->string());
            } else {
                state.set_status("Control socket failed: " + control_error);
                log_debug(
                    "control socket start failed path=" + state.config.control_socket_path->string() +
                    " error=" + control_error);
            }
        }
        if (!state.config.lsp_servers.empty()) {
            for (const LspServerConfig &server : state.config.lsp_servers) {
                state.runtime.add_service(std::make_unique<LspService>(server));
            }
        }
        render_frame(state);
        state.runtime.start_services();
        run_editor(state);
        state.runtime.stop_services();
        state.control_server.stop();
        teardown_terminal();
        return 0;
    } catch (const std::exception &error) {
        state.runtime.stop_services();
        state.control_server.stop();
        teardown_terminal();
        std::fprintf(stderr, "medit: %s\n", error.what());
        return 1;
    }
}
