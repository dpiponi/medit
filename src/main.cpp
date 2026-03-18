#include "editor_internal.hpp"
#include "config.hpp"
#include "keybindings.hpp"
#include "theme.hpp"
#include "services.hpp"
#include "lsp_service.hpp"
#include "logger.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>

import theme;

int main(int argc, char **argv) {
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
