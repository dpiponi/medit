CXX := c++
PYTHON ?= python3
UNAME_S := $(shell uname -s)
HAVE_PKGCONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && echo yes)

BASE_CPPFLAGS := -Isrc -Ithird_party -D_XOPEN_SOURCE_EXTENDED=1
BASE_CXXFLAGS := -std=c++23 -fmodules-ts -Wall -Wextra -pedantic
BASE_LDFLAGS :=
BASE_LDLIBS :=

ifeq ($(UNAME_S),Darwin)
BASE_CPPFLAGS += -D_DARWIN_C_SOURCE
else
BASE_LDLIBS += -ldl
endif

CURSES_CFLAGS :=
CURSES_LIBS :=

ifeq ($(HAVE_PKGCONFIG),yes)
CURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
CURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null)

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

CPPFLAGS := $(BASE_CPPFLAGS) $(CURSES_CFLAGS)
CXXFLAGS := $(BASE_CXXFLAGS)
LDFLAGS := $(BASE_LDFLAGS)
LDLIBS := $(BASE_LDLIBS) $(CURSES_LIBS)
SRC_DIR := src
TEST_DIR := tests
BUILD_DIR := build

THEME_MODULE_INTERFACE := $(SRC_DIR)/theme.cppm
THEME_MODULE_OBJECT := $(BUILD_DIR)/$(THEME_MODULE_INTERFACE).o
CORE_MODULE_INTERFACE := $(SRC_DIR)/editor_core.cppm
CORE_MODULE_OBJECT := $(BUILD_DIR)/$(CORE_MODULE_INTERFACE).o
APP_SOURCES := $(SRC_DIR)/main.cpp $(SRC_DIR)/editor.cpp $(SRC_DIR)/editor_ui.cpp $(SRC_DIR)/editor_input.cpp $(SRC_DIR)/editor_control.cpp $(SRC_DIR)/editor_ex_commands.cpp $(SRC_DIR)/editor_ex_command_completion.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/editor_commands.cpp $(SRC_DIR)/editor_session.cpp $(SRC_DIR)/editor_windows.cpp $(SRC_DIR)/clipboard.cpp $(SRC_DIR)/control_server.cpp $(SRC_DIR)/command_recording.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/logger.cpp $(SRC_DIR)/process_utils.cpp $(SRC_DIR)/string_utils.cpp $(SRC_DIR)/text_encoding_utils.cpp $(SRC_DIR)/position_utils.cpp $(SRC_DIR)/uri_utils.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/services.cpp $(SRC_DIR)/lsp_service.cpp $(SRC_DIR)/syntax.cpp
TEST_SOURCES := $(TEST_DIR)/test_editor_core.cpp $(SRC_DIR)/editor_ex_command_completion.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/editor_commands.cpp $(SRC_DIR)/editor_session.cpp $(SRC_DIR)/editor_windows.cpp $(SRC_DIR)/clipboard.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/logger.cpp $(SRC_DIR)/process_utils.cpp $(SRC_DIR)/string_utils.cpp $(SRC_DIR)/text_encoding_utils.cpp $(SRC_DIR)/position_utils.cpp $(SRC_DIR)/uri_utils.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/services.cpp $(SRC_DIR)/lsp_service.cpp $(SRC_DIR)/syntax.cpp
APP_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(APP_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SOURCES))
DEPFILES := $(APP_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(THEME_MODULE_OBJECT:.o=.d) $(CORE_MODULE_OBJECT:.o=.d)
THEME_IMPORTERS := $(BUILD_DIR)/src/main.o $(BUILD_DIR)/src/editor.o $(BUILD_DIR)/src/editor_ui.o $(BUILD_DIR)/tests/test_editor_core.o

all: medit

medit: $(CORE_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(APP_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(CORE_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(APP_OBJECTS) $(LDLIBS)

input_diag: tools/input_diag.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test_editor_core: $(CORE_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(TEST_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(CORE_MODULE_OBJECT) $(THEME_MODULE_OBJECT) $(TEST_OBJECTS) $(LDLIBS)

$(CORE_MODULE_OBJECT): $(CORE_MODULE_INTERFACE)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -x c++ -c $< -o $@

$(THEME_MODULE_OBJECT): $(THEME_MODULE_INTERFACE)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -x c++ -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/src/editor_core.o: $(CORE_MODULE_OBJECT)
$(BUILD_DIR)/tests/test_editor_core.o: $(CORE_MODULE_OBJECT)
$(BUILD_DIR)/src/theme.o: $(THEME_MODULE_OBJECT)
$(THEME_MODULE_OBJECT): $(CORE_MODULE_OBJECT)
$(THEME_IMPORTERS): $(THEME_MODULE_OBJECT)

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

.PHONY: all clean test bootstrap-tree-sitter tree-sitter-clean bootstrap-tree-sitter-%

-include $(DEPFILES)
