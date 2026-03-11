CXX := c++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic
UNAME_S := $(shell uname -s)
NCURSES_PKG := $(shell if pkg-config --exists ncursesw 2>/dev/null; then echo ncursesw; elif pkg-config --exists ncurses 2>/dev/null; then echo ncurses; fi)
NCURSES_CFLAGS := $(if $(NCURSES_PKG),$(shell pkg-config --cflags $(NCURSES_PKG) 2>/dev/null))
NCURSES_LIBS := $(if $(NCURSES_PKG),$(shell pkg-config --libs $(NCURSES_PKG) 2>/dev/null))
BREW_NCURSES_PREFIX := $(shell if command -v brew >/dev/null 2>&1; then brew --prefix ncurses 2>/dev/null; fi)
BREW_NCURSES_CFLAGS := $(if $(BREW_NCURSES_PREFIX),-I$(BREW_NCURSES_PREFIX)/include)
BREW_NCURSES_LIBS := $(if $(BREW_NCURSES_PREFIX),-L$(BREW_NCURSES_PREFIX)/lib -lncursesw)
CPPFLAGS := -Isrc $(if $(strip $(NCURSES_CFLAGS)),$(NCURSES_CFLAGS),$(if $(filter Darwin,$(UNAME_S)),$(BREW_NCURSES_CFLAGS)))
LDFLAGS := $(if $(strip $(NCURSES_LIBS)),$(NCURSES_LIBS),$(if $(filter Darwin,$(UNAME_S)),$(if $(BREW_NCURSES_PREFIX),$(BREW_NCURSES_LIBS),-lncursesw),-lncursesw))
SRC_DIR := src
TEST_DIR := tests

APP_SOURCES := $(SRC_DIR)/editor.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/editor_commands.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/services.cpp $(SRC_DIR)/lsp_service.cpp $(SRC_DIR)/syntax.cpp
TEST_SOURCES := $(TEST_DIR)/test_editor_core.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/editor_commands.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/services.cpp $(SRC_DIR)/lsp_service.cpp $(SRC_DIR)/syntax.cpp

all: medit

medit: $(APP_SOURCES)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(APP_SOURCES) $(LDFLAGS)

test_editor_core: $(TEST_SOURCES)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(TEST_SOURCES)

test: test_editor_core
	./test_editor_core

clean:
	rm -f medit test_editor_core

.PHONY: all clean test
