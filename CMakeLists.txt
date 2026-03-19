cmake_minimum_required(VERSION 3.15)
project(medit)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LUA QUIET lua5.4)
    if(NOT LUA_FOUND)
        pkg_check_modules(LUA QUIET lua-5.4)
    endif()
    if(NOT LUA_FOUND)
        pkg_check_modules(LUA QUIET lua54)
    endif()
    if(NOT LUA_FOUND)
        pkg_check_modules(LUA QUIET lua)
    endif()
endif()

# Platform detection
if(WIN32)
    add_definitions(-DPLATFORM_WINDOWS)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    # Disable some warnings for Windows
    if(MSVC)
        add_compile_options(/W3)
    else()
        add_compile_options(-Wall -Wextra -pedantic)
    endif()
else()
    add_definitions(-DPLATFORM_UNIX)
    add_definitions(-D_XOPEN_SOURCE_EXTENDED=1)
    add_compile_options(-Wall -Wextra -pedantic)
endif()

# Find curses library
if(WIN32)
    # For Windows, we need PDCurses
    # User should have PDCurses installed and set PDCURSES_DIR
    find_path(PDCURSES_INCLUDE_DIR curses.h
        HINTS ${PDCURSES_DIR}/include
        PATHS "C:/PDCurses" "C:/Program Files/PDCurses"
    )
    find_library(PDCURSES_LIBRARY
        NAMES pdcurses libpdcurses
        HINTS ${PDCURSES_DIR}/lib ${PDCURSES_DIR}/wincon
        PATHS "C:/PDCurses/wincon" "C:/Program Files/PDCurses/lib"
    )

    if(PDCURSES_INCLUDE_DIR AND PDCURSES_LIBRARY)
        set(CURSES_FOUND TRUE)
        set(CURSES_INCLUDE_DIRS ${PDCURSES_INCLUDE_DIR})
        set(CURSES_LIBRARIES ${PDCURSES_LIBRARY})
        message(STATUS "Found PDCurses: ${PDCURSES_LIBRARY}")
    else()
        message(FATAL_ERROR "PDCurses not found. Please install PDCurses and set PDCURSES_DIR")
    endif()
else()
    # For Unix, use ncurses
    find_package(Curses REQUIRED)
    if(NOT CURSES_HAVE_NCURSES_H)
        message(STATUS "Using curses.h instead of ncurses.h")
    endif()
endif()

include_directories(${CURSES_INCLUDE_DIRS})
include_directories(src)
include_directories(third_party)
if(LUA_FOUND)
    add_compile_definitions(MEDIT_HAS_LUA=1)
    include_directories(${LUA_INCLUDE_DIRS})
else()
    add_compile_definitions(MEDIT_HAS_LUA=0)
endif()

# Core editor sources (minimal set)
set(CORE_SOURCES
    src/main.cpp
    src/editor.cpp
    src/editor_core.cpp
    src/editor_commands.cpp
    src/editor_ex_command_completion.cpp
    src/editor_ex_commands.cpp
    src/editor_input.cpp
    src/command_recording.cpp
    src/editor_ui.cpp
    src/editor_windows.cpp
    src/editor_control.cpp
    src/editor_session.cpp
    src/config.cpp
    src/keybindings.cpp
    src/json.cpp
    src/logger.cpp
    src/lua_runtime.cpp
    src/theme.cpp
    src/syntax.cpp
    src/string_utils.cpp
    src/text_encoding_utils.cpp
    src/position_utils.cpp
    src/uri_utils.cpp
)

# Conditionally compiled sources
if(UNIX)
    # Unix-only features
    list(APPEND CORE_SOURCES
        src/control_server.cpp
        src/lsp_service.cpp
        src/clipboard.cpp
        src/process_utils.cpp
    )
else()
    # Windows stubs for disabled features
    list(APPEND CORE_SOURCES
        src/platform_stubs.cpp
    )
endif()

# Services is platform-independent
list(APPEND CORE_SOURCES src/services.cpp)

add_executable(medit ${CORE_SOURCES})

target_link_libraries(medit ${CURSES_LIBRARIES})
if(LUA_FOUND)
    target_link_libraries(medit ${LUA_LIBRARIES})
endif()

if(UNIX)
    target_link_libraries(medit dl)
endif()

# Install target
install(TARGETS medit DESTINATION bin)
