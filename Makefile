CXX := c++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic
CPPFLAGS := -Isrc
LDFLAGS := -lncursesw
SRC_DIR := src
TEST_DIR := tests

APP_SOURCES := $(SRC_DIR)/editor.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/theme.cpp
TEST_SOURCES := $(TEST_DIR)/test_editor_core.cpp $(SRC_DIR)/editor_core.cpp $(SRC_DIR)/keybindings.cpp $(SRC_DIR)/config.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/theme.cpp

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
