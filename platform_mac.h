#pragma once

// Mac shims replacing Windows.h / winmm: keyboard input and console API.

#include <termios.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <locale>

// ---------------------------------------------------------------------------
// Keyboard: raw-terminal key tracking
// ---------------------------------------------------------------------------

static std::map<int, std::chrono::steady_clock::time_point> g_keyLastSeen;
static std::mutex    g_keyMutex;
static struct termios g_origTermios;
static std::atomic<bool> g_rawMode{false};
static std::atomic<bool> g_keyboardStarted{false};

static void cleanupTerminal() {
    if (g_rawMode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_origTermios);
        g_rawMode = false;
    }
}

// Map terminal character to Windows-style VK code used by the synth key tables.
static int termCharToVK(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';   // uppercase = same VK
    if (c >= 'A' && c <= 'Z') return (int)c;
    if (c == ',') return 0xBC;   // VK_OEM_COMMA
    if (c == '.') return 0xBE;   // VK_OEM_PERIOD
    if (c == '/') return 0xBF;   // VK_OEM_2
    return 0;
}

static void startKeyboard() {
    bool expected = false;
    if (!g_keyboardStarted.compare_exchange_strong(expected, true)) return;

    // Save original terminal settings and register cleanup.
    tcgetattr(STDIN_FILENO, &g_origTermios);
    std::atexit(cleanupTerminal);

    // Set up wide-char output locale (needed for wcout on macOS).
    std::setlocale(LC_ALL, "");

    std::thread([] {
        struct termios raw = g_origTermios;
        raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        g_rawMode = true;

        while (true) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                int vk = termCharToVK(c);
                if (vk) {
                    std::lock_guard<std::mutex> lk(g_keyMutex);
                    g_keyLastSeen[vk] = std::chrono::steady_clock::now();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

// Returns 0x8000 if key was seen within the last 80ms (matches Windows semantics).
static short GetAsyncKeyState(int vk) {
    std::lock_guard<std::mutex> lk(g_keyMutex);
    auto it = g_keyLastSeen.find(vk);
    if (it == g_keyLastSeen.end()) return 0;
    auto elapsed = std::chrono::steady_clock::now() - it->second;
    return (elapsed < std::chrono::milliseconds(80)) ? (short)0x8000 : 0;
}

// ---------------------------------------------------------------------------
// Windows console API shims (used by main4.cpp)
// ---------------------------------------------------------------------------

typedef void*          HANDLE;
typedef unsigned long  DWORD;

struct COORD { short X; short Y; };

#define GENERIC_READ           0x80000000UL
#define GENERIC_WRITE          0x40000000UL
#define CONSOLE_TEXTMODE_BUFFER 1

inline HANDLE CreateConsoleScreenBuffer(DWORD, DWORD, void*, DWORD, void*) {
    // Hide cursor and clear screen once.
    printf("\033[?25l\033[2J");
    std::fflush(stdout);
    return (HANDLE)1;
}

inline void SetConsoleActiveScreenBuffer(HANDLE) {}

// Render a 80-wide wchar_t framebuffer to the terminal via ANSI escape.
inline bool WriteConsoleOutputCharacter(HANDLE, const wchar_t* buf, DWORD nLen,
                                        COORD, DWORD* written) {
    printf("\033[H"); // cursor to top-left
    for (DWORD i = 0; i < nLen; i++) {
        if (i > 0 && i % 80 == 0) putchar('\n');
        wchar_t ch = buf[i];
        putchar((ch >= 32 && ch < 127) ? (char)ch : ' ');
    }
    std::fflush(stdout);
    if (written) *written = nLen;
    return true;
}
