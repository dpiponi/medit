CXX := c++
UNAME_S := $(shell uname -s)
HAVE_PKGCONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && echo yes)

BASE_CPPFLAGS := -Isrc -D_XOPEN_SOURCE_EXTENDED=1
BASE_CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic
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

APP_SOURCES := $(SRC_DIR)/editor.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/editor_commands.cpp $(SRC_DIR)/editor_session.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/logger.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/services.cpp $(SRC_DIR)/lsp_service.cpp $(SRC_DIR)/syntax.cpp
TEST_SOURCES := $(TEST_DIR)/test_editor_core.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/editor_commands.cpp $(SRC_DIR)/editor_session.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/logger.cpp $(SRC_DIR)/theme.cpp $(SRC_DIR)/services.cpp $(SRC_DIR)/lsp_service.cpp $(SRC_DIR)/syntax.cpp
APP_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(APP_SOURCES))
TEST_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SOURCES))
DEPFILES := $(APP_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

all: medit

medit: $(APP_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(APP_OBJECTS) $(LDLIBS)

test_editor_core: $(TEST_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(TEST_OBJECTS) $(LDLIBS)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

test: test_editor_core
	./test_editor_core

clean:
	rm -rf $(BUILD_DIR) medit test_editor_core

.PHONY: all clean test

-include $(DEPFILES)
