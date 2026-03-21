ifeq ($(origin CXX),default)
CXX := $(or $(shell command -v clang++-20 2>/dev/null),$(shell if command -v brew >/dev/null 2>&1; then prefix=$$(brew --prefix llvm 2>/dev/null); if [ -x "$$prefix/bin/clang++" ]; then printf '%s' "$$prefix/bin/clang++"; fi; fi),clang++)
endif
ifeq ($(origin CXX),undefined)
CXX := $(or $(shell command -v clang++-20 2>/dev/null),$(shell if command -v brew >/dev/null 2>&1; then prefix=$$(brew --prefix llvm 2>/dev/null); if [ -x "$$prefix/bin/clang++" ]; then printf '%s' "$$prefix/bin/clang++"; fi; fi),clang++)
endif
PYTHON ?= python3
UNAME_S := $(shell uname -s)
HAVE_PKGCONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && echo yes)
CXX_VERSION_LINE := $(shell $(CXX) --version 2>/dev/null | sed -n '1p')
CXX_CLANG_MAJOR := $(shell $(CXX) --version 2>/dev/null | sed -n 's/.*clang version \([0-9][0-9]*\)\..*/\1/p' | sed -n '1p')
SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)

BASE_CPPFLAGS := -Isrc -Isrc/app -Isrc/editor -Isrc/core -Isrc/platform -Isrc/util -Ithird_party -D_XOPEN_SOURCE_EXTENDED=1
BASE_CXXFLAGS := -std=c++23 -Wall -Wextra -pedantic
BASE_LDFLAGS :=
BASE_LDLIBS :=

ifeq ($(UNAME_S),Darwin)
BASE_CPPFLAGS += -D_DARWIN_C_SOURCE
ifneq ($(strip $(SDKROOT)),)
BASE_CPPFLAGS += -isysroot $(SDKROOT)
BASE_LDFLAGS += -isysroot $(SDKROOT)
endif
else
BASE_LDLIBS += -ldl
endif

CURSES_CFLAGS :=
CURSES_LIBS :=
LUA_CFLAGS :=
LUA_LIBS :=
BREW_NCURSES_PREFIX := $(shell if command -v brew >/dev/null 2>&1; then brew --prefix ncurses 2>/dev/null; fi)
BREW_LUA54_PREFIX := $(shell if command -v brew >/dev/null 2>&1; then brew --prefix lua@5.4 2>/dev/null; fi)

ifneq ($(strip $(BREW_LUA54_PREFIX)),)
ifneq ($(wildcard $(BREW_LUA54_PREFIX)/lib/liblua.dylib),)
LUA_CFLAGS := -I$(BREW_LUA54_PREFIX)/include/lua5.4
LUA_LIBS := -L$(BREW_LUA54_PREFIX)/lib -llua -lm
endif
endif

ifeq ($(HAVE_PKGCONFIG),yes)
CURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
CURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null)
ifeq ($(strip $(LUA_LIBS)),)
LUA_CFLAGS := $(shell for pkg in lua5.4 lua-5.4 lua54 lua; do if pkg-config --exists $$pkg 2>/dev/null; then pkg-config --cflags $$pkg; break; fi; done)
LUA_LIBS := $(shell for pkg in lua5.4 lua-5.4 lua54 lua; do if pkg-config --exists $$pkg 2>/dev/null; then pkg-config --libs $$pkg; break; fi; done)
endif

ifeq ($(strip $(CURSES_LIBS)),)
CURSES_CFLAGS := $(shell pkg-config --cflags ncurses 2>/dev/null)
CURSES_LIBS := $(shell pkg-config --libs ncurses 2>/dev/null)
endif
endif

ifeq ($(strip $(CURSES_LIBS)),)
ifeq ($(UNAME_S),Darwin)
ifneq ($(strip $(BREW_NCURSES_PREFIX)),)
CURSES_CFLAGS := -I$(BREW_NCURSES_PREFIX)/include
CURSES_LIBS := -L$(BREW_NCURSES_PREFIX)/lib -lncursesw
else
CURSES_LIBS := -lncursesw
endif
else
CURSES_LIBS := -lncursesw
endif
endif

ifeq ($(strip $(LUA_LIBS)),)
BASE_CPPFLAGS += -DMEDIT_HAS_LUA=0
else
BASE_CPPFLAGS += -DMEDIT_HAS_LUA=1
endif

CPPFLAGS := $(BASE_CPPFLAGS) $(CURSES_CFLAGS) $(LUA_CFLAGS)
CXXFLAGS := $(BASE_CXXFLAGS)
LDFLAGS := $(BASE_LDFLAGS)
LDLIBS := $(BASE_LDLIBS) $(CURSES_LIBS) $(LUA_LIBS)
SRC_DIR := src
TEST_DIR := tests
BUILD_DIR := build
MODULE_DIR := $(BUILD_DIR)/modules

THEME_MODULE_INTERFACE := $(SRC_DIR)/core/theme.cppm
THEME_MODULE_OBJECT := $(BUILD_DIR)/$(THEME_MODULE_INTERFACE).o
THEME_MODULE_PCM := $(MODULE_DIR)/theme.pcm
CORE_MODULE_INTERFACE := $(SRC_DIR)/core/editor_core.cppm
CORE_MODULE_OBJECT := $(BUILD_DIR)/$(CORE_MODULE_INTERFACE).o
CORE_MODULE_PCM := $(MODULE_DIR)/editor_core.pcm
CLIPBOARD_MODULE_INTERFACE := $(SRC_DIR)/platform/clipboard.cppm
CLIPBOARD_MODULE_OBJECT := $(BUILD_DIR)/$(CLIPBOARD_MODULE_INTERFACE).o
CLIPBOARD_MODULE_PCM := $(MODULE_DIR)/clipboard.pcm
SESSION_MODULE_INTERFACE := $(SRC_DIR)/core/editor_session.cppm
SESSION_MODULE_OBJECT := $(BUILD_DIR)/$(SESSION_MODULE_INTERFACE).o
SESSION_MODULE_PCM := $(MODULE_DIR)/editor_session.pcm
COMMANDS_MODULE_INTERFACE := $(SRC_DIR)/core/editor_commands.cppm
COMMANDS_MODULE_OBJECT := $(BUILD_DIR)/$(COMMANDS_MODULE_INTERFACE).o
COMMANDS_MODULE_PCM := $(MODULE_DIR)/editor_commands.pcm
SERVICES_MODULE_INTERFACE := $(SRC_DIR)/core/services.cppm
SERVICES_MODULE_OBJECT := $(BUILD_DIR)/$(SERVICES_MODULE_INTERFACE).o
SERVICES_MODULE_PCM := $(MODULE_DIR)/services.pcm
CONFIG_MODULE_INTERFACE := $(SRC_DIR)/core/config.cppm
CONFIG_MODULE_OBJECT := $(BUILD_DIR)/$(CONFIG_MODULE_INTERFACE).o
CONFIG_MODULE_PCM := $(MODULE_DIR)/config.pcm
KEYBINDINGS_MODULE_INTERFACE := $(SRC_DIR)/core/keybindings.cppm
KEYBINDINGS_MODULE_OBJECT := $(BUILD_DIR)/$(KEYBINDINGS_MODULE_INTERFACE).o
KEYBINDINGS_MODULE_PCM := $(MODULE_DIR)/keybindings.pcm
APP_SOURCES := $(SRC_DIR)/app/main.cpp $(SRC_DIR)/editor/editor.cpp $(SRC_DIR)/editor/editor_ui.cpp $(SRC_DIR)/editor/editor_input.cpp $(SRC_DIR)/editor/editor_control.cpp $(SRC_DIR)/editor/editor_ex_commands.cpp $(SRC_DIR)/editor/editor_ex_command_completion.cpp $(SRC_DIR)/core/editor_core.cpp $(SRC_DIR)/core/editor_commands.cpp $(SRC_DIR)/core/editor_session.cpp $(SRC_DIR)/editor/editor_windows.cpp $(SRC_DIR)/platform/clipboard.cpp $(SRC_DIR)/platform/control_server.cpp $(SRC_DIR)/editor/command_recording.cpp $(SRC_DIR)/core/config.cpp $(SRC_DIR)/util/json.cpp $(SRC_DIR)/util/logger.cpp $(SRC_DIR)/platform/lua_runtime.cpp $(SRC_DIR)/util/process_utils.cpp $(SRC_DIR)/util/string_utils.cpp $(SRC_DIR)/util/text_encoding_utils.cpp $(SRC_DIR)/util/position_utils.cpp $(SRC_DIR)/util/uri_utils.cpp $(SRC_DIR)/core/theme.cpp $(SRC_DIR)/core/services.cpp $(SRC_DIR)/platform/lsp_service.cpp $(SRC_DIR)/core/syntax.cpp
TEST_SOURCES := $(TEST_DIR)/test_editor_core.cpp $(SRC_DIR)/editor/editor.cpp $(SRC_DIR)/editor/editor_ui.cpp $(SRC_DIR)/editor/editor_input.cpp $(SRC_DIR)/editor/editor_control.cpp $(SRC_DIR)/editor/editor_ex_commands.cpp $(SRC_DIR)/editor/editor_ex_command_completion.cpp $(SRC_DIR)/core/editor_core.cpp $(SRC_DIR)/core/editor_commands.cpp $(SRC_DIR)/core/editor_session.cpp $(SRC_DIR)/editor/editor_windows.cpp $(SRC_DIR)/platform/clipboard.cpp $(SRC_DIR)/platform/control_server.cpp $(SRC_DIR)/editor/command_recording.cpp $(SRC_DIR)/core/config.cpp $(SRC_DIR)/util/json.cpp $(SRC_DIR)/util/logger.cpp $(SRC_DIR)/platform/lua_runtime.cpp $(SRC_DIR)/util/process_utils.cpp $(SRC_DIR)/util/string_utils.cpp $(SRC_DIR)/util/text_encoding_utils.cpp $(SRC_DIR)/util/position_utils.cpp $(SRC_DIR)/util/uri_utils.cpp $(SRC_DIR)/core/theme.cpp $(SRC_DIR)/core/services.cpp $(SRC_DIR)/platform/lsp_service.cpp $(SRC_DIR)/core/syntax.cpp
APP_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(APP_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SOURCES))
SHARED_OBJECTS := $(sort $(APP_OBJECTS) $(TEST_OBJECTS))
DEPFILES := $(APP_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(THEME_MODULE_OBJECT:.o=.d) $(CORE_MODULE_OBJECT:.o=.d) $(CLIPBOARD_MODULE_OBJECT:.o=.d) $(SESSION_MODULE_OBJECT:.o=.d) $(COMMANDS_MODULE_OBJECT:.o=.d) $(SERVICES_MODULE_OBJECT:.o=.d) $(CONFIG_MODULE_OBJECT:.o=.d) $(KEYBINDINGS_MODULE_OBJECT:.o=.d)
MODULE_PREBUILT_FLAGS := -fprebuilt-module-path=$(MODULE_DIR)

all: medit

medit: $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(KEYBINDINGS_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(APP_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(KEYBINDINGS_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(APP_OBJECTS) $(LDLIBS)

input_diag: tools/input_diag.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test_editor_core: $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(KEYBINDINGS_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(TEST_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(KEYBINDINGS_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(TEST_OBJECTS) $(LDLIBS)

check-toolchain:
	@if ! command -v "$(CXX)" >/dev/null 2>&1 && [ ! -x "$(CXX)" ]; then \
		echo "error: CXX=$(CXX) was not found"; \
		echo "hint: install LLVM clang 20 or newer, or run make CXX=/path/to/clang++"; \
		exit 1; \
	fi
	@if printf '%s\n' "$(CXX_VERSION_LINE)" | grep -q "Apple clang"; then \
		echo "error: $(CXX) resolves to Apple Clang, which does not build this project's C++ module targets"; \
		echo "hint: install Homebrew llvm and run make CXX=/opt/homebrew/opt/llvm/bin/clang++"; \
		exit 1; \
	fi
	@if [ -z "$(CXX_CLANG_MAJOR)" ] || [ "$(CXX_CLANG_MAJOR)" -lt 20 ]; then \
		echo "error: $(CXX) must be Clang 20 or newer for this Makefile"; \
		echo "detected: $(CXX_VERSION_LINE)"; \
		exit 1; \
	fi

$(CORE_MODULE_OBJECT): $(CORE_MODULE_INTERFACE) | check-toolchain
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -fmodule-output=$(CORE_MODULE_PCM) -c $< -o $@

$(CLIPBOARD_MODULE_OBJECT): $(CLIPBOARD_MODULE_INTERFACE) | check-toolchain
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(CLIPBOARD_MODULE_PCM) -c $< -o $@

$(SESSION_MODULE_OBJECT): $(SESSION_MODULE_INTERFACE) | check-toolchain
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(SESSION_MODULE_PCM) -c $< -o $@

$(COMMANDS_MODULE_OBJECT): $(COMMANDS_MODULE_INTERFACE) | check-toolchain
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(COMMANDS_MODULE_PCM) -c $< -o $@

$(SERVICES_MODULE_OBJECT): $(SERVICES_MODULE_INTERFACE) | check-toolchain
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(SERVICES_MODULE_PCM) -c $< -o $@

$(CONFIG_MODULE_OBJECT): $(CONFIG_MODULE_INTERFACE) | check-toolchain
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(CONFIG_MODULE_PCM) -c $< -o $@

$(KEYBINDINGS_MODULE_OBJECT): $(KEYBINDINGS_MODULE_INTERFACE) | check-toolchain
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(KEYBINDINGS_MODULE_PCM) -c $< -o $@

$(THEME_MODULE_OBJECT): $(THEME_MODULE_INTERFACE) | check-toolchain
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(THEME_MODULE_PCM) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp | check-toolchain
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_FLAGS) -MMD -MP -c $< -o $@

$(SHARED_OBJECTS): MODULE_FLAGS += $(MODULE_PREBUILT_FLAGS)
$(SHARED_OBJECTS): $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(KEYBINDINGS_MODULE_OBJECT) $(THEME_MODULE_OBJECT)

$(CLIPBOARD_MODULE_OBJECT): $(CORE_MODULE_OBJECT)
$(SESSION_MODULE_OBJECT): $(CLIPBOARD_MODULE_OBJECT) $(CORE_MODULE_OBJECT)
$(COMMANDS_MODULE_OBJECT): $(CORE_MODULE_OBJECT)
$(SERVICES_MODULE_OBJECT): $(COMMANDS_MODULE_OBJECT) $(CORE_MODULE_OBJECT)
$(CONFIG_MODULE_OBJECT): $(CLIPBOARD_MODULE_OBJECT)
$(KEYBINDINGS_MODULE_OBJECT): $(CONFIG_MODULE_OBJECT)
$(THEME_MODULE_OBJECT): $(CONFIG_MODULE_OBJECT) $(CORE_MODULE_OBJECT)

editor_core_module: $(CORE_MODULE_OBJECT)

test: test_editor_core
	./test_editor_core

bootstrap-tree-sitter:
	$(PYTHON) tools/bootstrap_tree_sitter.py --manifest tools/tree_sitter_languages.json --config-root $(CONFIG_ROOT)/medit --build-root $(BUILD_DIR)/tree-sitter

bootstrap-tree-sitter-%:
	$(PYTHON) tools/bootstrap_tree_sitter.py --manifest tools/tree_sitter_languages.json --config-root $(CONFIG_ROOT)/medit --build-root $(BUILD_DIR)/tree-sitter --languages $*

tree-sitter-clean:
	rm -rf $(BUILD_DIR)/tree-sitter .config/medit/grammars .config/medit/queries .config/medit/libtree-sitter.so .config/medit/libtree-sitter.dylib .config/medit/syntax.json

clean:
	rm -rf $(BUILD_DIR) gcm.cache medit input_diag test_editor_core

CONFIG_ROOT ?= .config

.PHONY: all clean test bootstrap-tree-sitter tree-sitter-clean bootstrap-tree-sitter-% editor_core_module check-toolchain

-include $(DEPFILES)
