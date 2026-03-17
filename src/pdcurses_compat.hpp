#pragma once

// PDCurses compatibility layer for ncurses functions
// PDCurses doesn't support some wide character functions, so we provide wrappers

#ifdef _WIN32

// PDCurses doesn't have KEY_CODE_YES, but it returns OK/ERR
#ifndef KEY_CODE_YES
#define KEY_CODE_YES 0
#endif

// Wide character input wrapper
// PDCurses uses regular getch() which returns int
inline int get_wch(wint_t *wch) {
    int ch = getch();
    if (ch == ERR) {
        return ERR;
    }
    *wch = static_cast<wint_t>(ch);
    // PDCurses returns KEY_* values directly
    if (ch >= KEY_MIN && ch <= KEY_MAX) {
        return KEY_CODE_YES;
    }
    return OK;
}

// Wide character ungetch wrapper
inline int unget_wch(wint_t wch) {
    return ungetch(static_cast<int>(wch));
}

// Wide character string output wrapper
// PDCurses doesn't have mvaddnwstr, so convert wchar_t to char
inline int mvaddnwstr(int y, int x, const wchar_t *wstr, int n) {
    move(y, x);
    // For PDCurses on Windows, we can use wide char functions
    // PDCurses has addwstr but not mvaddnwstr
    for (int i = 0; i < n && wstr[i] != L'\0'; ++i) {
        if (wstr[i] < 256) {
            addch(static_cast<chtype>(wstr[i]));
        } else {
            // For non-ASCII, use a placeholder
            addch('?');
        }
    }
    return OK;
}

// Escape delay setting wrapper
// PDCurses doesn't have set_escdelay
inline void set_escdelay(int delay) {
    // PDCurses doesn't support this - just ignore it
    (void)delay;
}

// Mouse event wrapper
// PDCurses getmouse returns mmask_t and uses global state
// ncurses getmouse takes MEVENT* and returns int
inline int getmouse(MEVENT *event) {
    request_mouse_pos();
    if (event) {
        event->id = 0;
        event->x = Mouse_status.x;
        event->y = Mouse_status.y;
        event->z = 0;
        event->bstate = Mouse_status.changes;
    }
    return OK;
}

// Wide character width function
// Windows doesn't have wcwidth, so provide a simple implementation
inline int wcwidth(wchar_t wc) {
    // Basic implementation - most printable characters are width 1
    // CJK characters and other wide chars would be width 2
    if (wc == 0) return 0;
    if (wc < 0x20) return -1;  // Control characters
    if (wc >= 0x7F && wc < 0xA0) return -1;  // DEL and C1 controls

    // CJK and other wide character ranges (width 2)
    if ((wc >= 0x1100 && wc <= 0x115F) ||  // Hangul Jamo
        (wc >= 0x2329 && wc <= 0x232A) ||  // Left/Right-Pointing Angle Bracket
        (wc >= 0x2E80 && wc <= 0xA4CF && wc != 0x303F) ||  // CJK
        (wc >= 0xAC00 && wc <= 0xD7A3) ||  // Hangul Syllables
        (wc >= 0xF900 && wc <= 0xFAFF) ||  // CJK Compatibility Ideographs
        (wc >= 0xFE10 && wc <= 0xFE19) ||  // Vertical forms
        (wc >= 0xFE30 && wc <= 0xFE6F) ||  // CJK Compatibility Forms
        (wc >= 0xFF00 && wc <= 0xFF60) ||  // Fullwidth Forms
        (wc >= 0xFFE0 && wc <= 0xFFE6) ||  // Fullwidth Forms
        (wc >= 0x20000 && wc <= 0x2FFFD) ||  // CJK Ideographs Extension
        (wc >= 0x30000 && wc <= 0x3FFFD)) {
        return 2;
    }

    return 1;  // Default: most characters are width 1
}

#endif // _WIN32
