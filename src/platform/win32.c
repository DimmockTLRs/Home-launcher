#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>
#include <wchar.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include "../launcher.h"
#include <launcher_config.h>
#include "platform.h"
#include "../util.h"
#include "../debug.h"
#include "slideshow.h"

static UINT sdl_to_win32_keycode(SDL_Keycode keycode);
static bool get_shutdown_privilege(void);

extern Config config;
extern SDL_SysWMinfo wm_info;
bool has_shutdown_privilege     = false;
UINT exit_hotkey                = 0;


// A function to determine if a file exists on the filesystem
bool file_exists(const char *path)
{
    return _access(path, 4) ? false : true;
}

// A function that parses the command string into a file and parameters
static bool parse_command(const char *cmd, char *file, size_t file_size, const char **params)
{
    const char *start = cmd + strspn(cmd, " \t");
    const char *end;
    const char *rest;
    if (*start == '"') {
        start++;
        end = strchr(start, '"');
        if (end == NULL || (end[1] != '\0' && end[1] != ' ' && end[1] != '\t'))
            return false;
        rest = end + 1;
    }
    else {
        end = start + strcspn(start, " \t");
        rest = end;
    }

    size_t length = (size_t)(end - start);
    if (length == 0 || length >= file_size)
        return false;
    memcpy(file, start, length);
    file[length] = '\0';
    rest += strspn(rest, " \t");
    *params = *rest != '\0' ? rest : NULL;
    return true;
}

void debug_foreground_window(void)
{
    if (!config.debug)
        return;

    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground != NULL)
        GetWindowThreadProcessId(foreground, &pid);
    log_debug("Foreground window=%p pid=%lu launcher=%s",
        (void *)foreground, (unsigned long)pid,
        foreground != NULL && foreground == wm_info.info.win.window ? "yes" : "no");
}

void set_foreground_window()
{
    SetForegroundWindow(wm_info.info.win.window);
}

void make_window_transparent()
{
    HWND hwnd = wm_info.info.win.window;
    SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd,
        RGB(config.chroma_key_color.r, config.chroma_key_color.g, config.chroma_key_color.b),
        0,
        LWA_COLORKEY
    );
}

// When the window is transparent, we need to hide the cursor behind the non-transparent icon
void hide_cursor(Entry *entry)
{
    SetCursorPos(entry->icon_rect.x + entry->icon_rect.w / 2,
        entry->icon_rect.y + entry->icon_rect.h / 2
    );
}

// A function to launch an application
bool start_process(char *cmd, bool application)
{
    char file[MAX_PATH_CHARS + 1];
    const char *params = NULL;
    if (!parse_command(cmd, file, sizeof(file), &params)) {
        log_error("Could not parse the launch target; check path length and quoting");
        return false;
    }

    SHELLEXECUTEINFOA info = {
        .cbSize = sizeof(SHELLEXECUTEINFOA),
        .fMask = application ? SEE_MASK_NOCLOSEPROCESS : 0,
        .lpVerb = "open",
        .lpFile = file,
        .lpParameters = params,
        .nShow = application ? SW_SHOWMAXIMIZED : SW_HIDE,
    };

    log_debug("Starting Windows shell activation for '%s' (tracked=%s)",
        file, application ? "yes" : "no");
    Uint32 started = SDL_GetTicks();
    BOOL successful = ShellExecuteExA(&info);
    DWORD error = successful ? ERROR_SUCCESS : GetLastError();
    Uint32 elapsed = SDL_GetTicks() - started;
    if (!successful) {
        log_error("Windows shell activation failed for '%s': Win32 error %lu after %u ms",
            file, (unsigned long)error, (unsigned)elapsed);
        return false;
    }

    DWORD pid = 0;
    if (info.hProcess != NULL) {
        pid = GetProcessId(info.hProcess);
        CloseHandle(info.hProcess);
    }
    log_debug("Windows shell accepted launch in %u ms (process ID %lu; 0 means unavailable)",
        (unsigned)elapsed, (unsigned long)pid);

    // Let the launched window come forward. Shell activation may reuse an existing process.
    if (application && wm_info.info.win.window != NULL &&
        !SetWindowPos(wm_info.info.win.window, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOREDRAW | SWP_NOSIZE | SWP_NOMOVE)) {
        log_debug("Could not lower launcher window: Win32 error %lu", (unsigned long)GetLastError());
    }
    return true;
}

// Enumerate UTF-8 folder paths using Windows' Unicode filesystem API.
bool scan_slideshow_directory(PhotoList *files, const char *directory, SDL_atomic_t *cancel)
{
    size_t length = strlen(directory);
    size_t size = length + 3;
    char *pattern = malloc(size);
    if (pattern == NULL)
        return false;
    const char *separator = length && (directory[length - 1] == '\\' ||
        directory[length - 1] == '/') ? "" : "\\";
    SDL_snprintf(pattern, size, "%s%s*", directory, separator);
    wchar_t *wide_pattern = (wchar_t *)SDL_iconv_string("UTF-16LE", "UTF-8", pattern, strlen(pattern) + 1);
    free(pattern);
    if (wide_pattern == NULL)
        return false;

    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(wide_pattern, &entry);
    DWORD error = search == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    SDL_free(wide_pattern);
    if (search == INVALID_HANDLE_VALUE)
        return error == ERROR_FILE_NOT_FOUND;

    bool success = true;
    while (!SDL_AtomicGet(cancel)) {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            size_t bytes = (wcslen(entry.cFileName) + 1) * sizeof(wchar_t);
            char *name = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)entry.cFileName, bytes);
            if (name == NULL) {
                success = false;
                break;
            }
            success = !is_photo_filename(name) || append_photo_path(files, directory, name);
            SDL_free(name);
            if (!success)
                break;
        }
        if (!FindNextFileW(search, &entry)) {
            success = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    }
    FindClose(search);
    return success;
}

// A function to get the 2 letter region code
void get_region(char *buffer)
{
    GEOID geo_id = GetUserGeoID(GEOCLASS_NATION);
    GetGeoInfoA(geo_id, GEO_ISO2, buffer, 3, 0);
}

// A function to shutdown the computer
void scmd_shutdown()
{
    if (!has_shutdown_privilege) {
        bool successful = get_shutdown_privilege();
        if (!successful) 
            return;
    }
    InitiateShutdownA(NULL, 
        NULL, 
        0, 
        SHUTDOWN_FORCE_OTHERS | SHUTDOWN_POWEROFF | SHUTDOWN_HYBRID,
        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER
    );
}

// A function to restart the computer
void scmd_restart()
{
    if (!has_shutdown_privilege) {
        bool successful = get_shutdown_privilege();
        if (!successful) 
            return;
    }
    InitiateShutdownA(NULL,
        NULL,
        0,
        SHUTDOWN_FORCE_OTHERS | SHUTDOWN_RESTART | SHUTDOWN_HYBRID,
        SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER
    );
}

// A function to put the computer to sleep
void scmd_sleep()
{
    if (!has_shutdown_privilege) {
        bool successful = get_shutdown_privilege();
        if (!successful) 
            return;
    }
    SetSuspendState(FALSE, FALSE, FALSE);
}

// A function to get the shutdown privilege from Windows
static bool get_shutdown_privilege()
{
    HANDLE token = NULL;
    BOOL ret = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token);
    if (!ret) {
        log_error("Could not open process token");
        return false;
    }
    LUID luid;
    ret = LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &luid);
    if (!ret) {
        log_error("Failed to lookup privilege");
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ret = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
    if (!ret) {
        log_error("Failed to adjust token privileges");
        CloseHandle(token);
        return false;
    }
    has_shutdown_privilege = true;
    CloseHandle(token);
    return true;
}

// A function to check if there is an exit hotkey
bool has_exit_hotkey()
{
    if (exit_hotkey) 
        return true;
    return false;
}

// A function to store an exit hotkey
void set_exit_hotkey(SDL_Keycode keycode)
{
    if (exit_hotkey) 
        return;
    exit_hotkey = sdl_to_win32_keycode(keycode);
    if (!exit_hotkey)
        log_error("Invalid exit hotkey keycode %X", keycode);
}

// A function to register the exit hotkey with Windows
void register_exit_hotkey()
{
    BOOL ret = RegisterHotKey(wm_info.info.win.window, 1, 0, exit_hotkey);
    if (!ret) {
        exit_hotkey = 0;
        log_error("Failed to register exit hotkey with Windows");
    }
}

// A function to check if the exit hotkey was pressed, and close the active window if so
void check_exit_hotkey(SDL_SysWMmsg *msg)
{
    if (msg->msg.win.msg == WM_HOTKEY) {
        log_debug("Exit hotkey detected");
        HWND hwnd = GetForegroundWindow();
        if (hwnd == NULL) {
            log_error("Could not get top window");
            return;
        }
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    }
}

// A function to convert an SDL keycode to a WIN32 virtual keycode
static UINT sdl_to_win32_keycode(SDL_Keycode keycode)
{
#include "keycode_convert.h" // Import the conversion table
    for (int i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].sdl == keycode)
            return table[i].win;
    }
    return 0;
}
