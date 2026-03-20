CXX := clang++-20
PYTHON ?= python3
UNAME_S := $(shell uname -s)
HAVE_PKGCONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && echo yes)

BASE_CPPFLAGS := -Isrc -Ithird_party -D_XOPEN_SOURCE_EXTENDED=1
BASE_CXXFLAGS := -std=c++23 -Wall -Wextra -pedantic
BASE_LDFLAGS :=
BASE_LDLIBS :=

ifeq ($(UNAME_S),Darwin)
BASE_CPPFLAGS += -D_DARWIN_C_SOURCE
else
BASE_LDLIBS += -ldl
endif

CURSES_CFLAGS :=
CURSES_LIBS :=
LUA_CFLAGS :=
LUA_LIBS :=

ifeq ($(HAVE_PKGCONFIG),yes)
CURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
CURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null)
LUA_CFLAGS := $(shell for pkg in lua5.4 lua-5.4 lua54 lua; do if pkg-config --exists $$pkg 2>/dev/null; then pkg-config --cflags $$pkg; break; fi; done)
LUA_LIBS := $(shell for pkg in lua5.4 lua-5.4 lua54 lua; do if pkg-config --exists $$pkg 2>/dev/null; then pkg-config --libs $$pkg; break; fi; done)

ifeq ($(strip $(CURSES_LIBS)),)
CURSES_CFLAGS := $(shell pkg-config --cflags ncurses 2>/dev/null)
CURSES_LIBS := $(shell pkg-config --libs ncurses 2>/dev/null)
endif
endif

BREW_NCURSES_PREFIX := $(shell if command -v brew >/dev/null 2>&1; then brew --prefix ncurses 2>/dev/null; fi)

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

THEME_MODULE_INTERFACE := $(SRC_DIR)/theme.cppm
THEME_MODULE_OBJECT := $(BUILD_DIR)/$(THEME_MODULE_INTERFACE).o
THEME_MODULE_PCM := $(MODULE_DIR)/theme.pcm
CORE_MODULE_INTERFACE := $(SRC_DIR)/editor_core.cppm
CORE_MODULE_OBJECT := $(BUILD_DIR)/$(CORE_MODULE_INTERFACE).o
CORE_MODULE_PCM := $(MODULE_DIR)/editor_core.pcm
CLIPBOARD_MODULE_INTERFACE := $(SRC_DIR)/clipboard.cppm
CLIPBOARD_MODULE_OBJECT := $(BUILD_DIR)/$(CLIPBOARD_MODULE_INTERFACE).o
CLIPBOARD_MODULE_PCM := $(MODULE_DIR)/clipboard.pcm
SESSION_MODULE_INTERFACE := $(SRC_DIR)/editor_session.cppm
SESSION_MODULE_OBJECT := $(BUILD_DIR)/$(SESSION_MODULE_INTERFACE).o
SESSION_MODULE_PCM := $(MODULE_DIR)/editor_session.pcm
COMMANDS_MODULE_INTERFACE := $(SRC_DIR)/editor_commands.cppm
COMMANDS_MODULE_OBJECT := $(BUILD_DIR)/$(COMMANDS_MODULE_INTERFACE).o
COMMANDS_MODULE_PCM := $(MODULE_DIR)/editor_commands.pcm
SERVICES_MODULE_INTERFACE := $(SRC_DIR)/services.cppm
SERVICES_MODULE_OBJECT := $(BUILD_DIR)/$(SERVICES_MODULE_INTERFACE).o
SERVICES_MODULE_PCM := $(MODULE_DIR)/services.pcm
CONFIG_MODULE_INTERFACE := $(SRC_DIR)/config.cppm
CONFIG_MODULE_OBJECT := $(BUILD_DIR)/$(CONFIG_MODULE_INTERFACE).o
CONFIG_MODULE_PCM := $(MODULE_DIR)/config.pcm
APP_SOURCES := $(SRC_DIR)/main.cpp $(SRC_DIR)/editor.cpp $(SRC_DIR)/editor_ui.cpp $(SRC_DIR)/editor_input.cpp $(SRC_DIR)/editor_control.cpp $(SRC_DIR)/editor_ex_commands.cpp $(SRC_DIR)/editor_ex_command_completion.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/editor_commands.cpp $(SRC_DIR)/editor_session.cpp $(SRC_DIR)/editor_windows.cpp $(SRC_DIR)/clipboard.cpp $(SRC_DIR)/control_server.cpp $(SRC_DIR)/command_recording.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/logger.cpp $(SRC_DIR)/lua_runtime.cpp $(SRC_DIR)/process_utils.cpp $(SRC_DIR)/string_utils.cpp $(SRC_DIR)/text_encoding_utils.cpp $(SRC_DIR)/position_utils.cpp $(SRC_DIR)/uri_utils.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/services.cpp $(SRC_DIR)/lsp_service.cpp $(SRC_DIR)/syntax.cpp
TEST_SOURCES := $(TEST_DIR)/test_editor_core.cpp $(SRC_DIR)/editor.cpp $(SRC_DIR)/editor_ui.cpp $(SRC_DIR)/editor_input.cpp $(SRC_DIR)/editor_control.cpp $(SRC_DIR)/editor_ex_commands.cpp $(SRC_DIR)/editor_ex_command_completion.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/editor_commands.cpp $(SRC_DIR)/editor_session.cpp $(SRC_DIR)/editor_windows.cpp $(SRC_DIR)/clipboard.cpp $(SRC_DIR)/control_server.cpp $(SRC_DIR)/command_recording.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/logger.cpp $(SRC_DIR)/lua_runtime.cpp $(SRC_DIR)/process_utils.cpp $(SRC_DIR)/string_utils.cpp $(SRC_DIR)/text_encoding_utils.cpp $(SRC_DIR)/position_utils.cpp $(SRC_DIR)/uri_utils.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/services.cpp $(SRC_DIR)/lsp_service.cpp $(SRC_DIR)/syntax.cpp
APP_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(APP_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SOURCES))
SHARED_OBJECTS := $(sort $(APP_OBJECTS) $(TEST_OBJECTS))
DEPFILES := $(APP_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(THEME_MODULE_OBJECT:.o=.d) $(CORE_MODULE_OBJECT:.o=.d) $(CLIPBOARD_MODULE_OBJECT:.o=.d) $(SESSION_MODULE_OBJECT:.o=.d) $(COMMANDS_MODULE_OBJECT:.o=.d) $(SERVICES_MODULE_OBJECT:.o=.d) $(CONFIG_MODULE_OBJECT:.o=.d)
MODULE_PREBUILT_FLAGS := -fprebuilt-module-path=$(MODULE_DIR)

all: medit

medit: $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(APP_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(APP_OBJECTS) $(LDLIBS)

input_diag: tools/input_diag.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test_editor_core: $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(TEST_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(CORE_MODULE_OBJECT) $(CLIPBOARD_MODULE_OBJECT) $(SESSION_MODULE_OBJECT) $(COMMANDS_MODULE_OBJECT) $(SERVICES_MODULE_OBJECT) $(CONFIG_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(TEST_OBJECTS) $(LDLIBS)

$(CORE_MODULE_OBJECT): $(CORE_MODULE_INTERFACE)
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -fmodule-output=$(CORE_MODULE_PCM) -c $< -o $@

$(CLIPBOARD_MODULE_OBJECT): $(CLIPBOARD_MODULE_INTERFACE)
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(CLIPBOARD_MODULE_PCM) -c $< -o $@

$(SESSION_MODULE_OBJECT): $(SESSION_MODULE_INTERFACE)
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(SESSION_MODULE_PCM) -c $< -o $@

$(COMMANDS_MODULE_OBJECT): $(COMMANDS_MODULE_INTERFACE)
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(COMMANDS_MODULE_PCM) -c $< -o $@

$(SERVICES_MODULE_OBJECT): $(SERVICES_MODULE_INTERFACE)
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(SERVICES_MODULE_PCM) -c $< -o $@

$(CONFIG_MODULE_OBJECT): $(CONFIG_MODULE_INTERFACE)
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(CONFIG_MODULE_PCM) -c $< -o $@

$(THEME_MODULE_OBJECT): $(THEME_MODULE_INTERFACE)
	@mkdir -p $(dir $@) $(MODULE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_PREBUILT_FLAGS) -MMD -MP -fmodule-output=$(THEME_MODULE_PCM) -c $< -o $@

$(CORE_MODULE_PCM): $(CORE_MODULE_OBJECT)
$(CLIPBOARD_MODULE_PCM): $(CLIPBOARD_MODULE_OBJECT)
$(SESSION_MODULE_PCM): $(SESSION_MODULE_OBJECT)
$(COMMANDS_MODULE_PCM): $(COMMANDS_MODULE_OBJECT)
$(SERVICES_MODULE_PCM): $(SERVICES_MODULE_OBJECT)
$(CONFIG_MODULE_PCM): $(CONFIG_MODULE_OBJECT)
$(THEME_MODULE_PCM): $(THEME_MODULE_OBJECT)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_FLAGS) -MMD -MP -c $< -o $@

$(SHARED_OBJECTS): MODULE_FLAGS += $(MODULE_PREBUILT_FLAGS)
$(SHARED_OBJECTS): $(CORE_MODULE_PCM) $(CLIPBOARD_MODULE_PCM) $(SESSION_MODULE_PCM) $(COMMANDS_MODULE_PCM) $(SERVICES_MODULE_PCM) $(CONFIG_MODULE_PCM) $(THEME_MODULE_PCM)

$(CLIPBOARD_MODULE_OBJECT): $(CORE_MODULE_PCM)
$(SESSION_MODULE_OBJECT): $(CLIPBOARD_MODULE_PCM) $(CORE_MODULE_PCM)
$(COMMANDS_MODULE_OBJECT): $(CORE_MODULE_PCM)
$(SERVICES_MODULE_OBJECT): $(COMMANDS_MODULE_PCM) $(CORE_MODULE_PCM)
$(CONFIG_MODULE_OBJECT): $(CLIPBOARD_MODULE_PCM)
$(THEME_MODULE_OBJECT): $(CONFIG_MODULE_PCM) $(CORE_MODULE_PCM)

editor_core_module: $(CORE_MODULE_PCM)

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

.PHONY: all clean test bootstrap-tree-sitter tree-sitter-clean bootstrap-tree-sitter-% editor_core_module

-include $(DEPFILES)
