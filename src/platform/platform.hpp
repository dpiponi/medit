#pragma once

// Platform abstraction layer for Windows/Unix compatibility
// Minimal version - just what's needed for basic editor

#include <string>

#ifdef _WIN32
    #define PLATFORM_WINDOWS 1
    #include <windows.h>
    #include <io.h>
    #define popen _popen
    #define pclose _pclose
    #define fileno _fileno
    #define isatty _isatty
#else
    #define PLATFORM_UNIX 1
    #include <unistd.h>
#endif

namespace platform {
    // Sleep for milliseconds
    inline void sleep_ms(int milliseconds) {
#ifdef _WIN32
        Sleep(milliseconds);
#else
        usleep(milliseconds * 1000);
#endif
    }

    // Check if file descriptor is a terminal
    inline bool is_terminal(int fd) {
        return isatty(fd) != 0;
    }
}
