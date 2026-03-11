CXX := c++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic
NCURSESW_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
NCURSESW_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null)
CPPFLAGS := -Isrc $(NCURSESW_CFLAGS)
LDFLAGS := $(if $(strip $(NCURSESW_LIBS)),$(NCURSESW_LIBS),-lncursesw)
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
