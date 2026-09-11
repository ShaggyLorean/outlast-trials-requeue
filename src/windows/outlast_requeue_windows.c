#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <tlhelp32.h>

#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "requeue_engine.h"
#include "resource.h"

#define APP_NAME L"Outlast Requeue"
#define APP_CLASS L"OutlastRequeue.Native.Window.v1"
#define APP_MUTEX L"Local\\OutlastRequeue.Native.SingleInstance.v1"
#define APP_VERSION L"1.0.4"
#define APP_AUTHOR L"whispersgone"
#define STEAM_APP_ID L"1304930"
#define SUPPORTED_BUILD_ID L"25112110"
#define GAME_EXE L"TOTClient-Win64-Shipping.exe"

#define TIMER_UI 1
#define TIMER_COLLAPSE 2
#define WM_STATE_CHANGED (WM_APP + 2)
#define WM_UI_EVENT (WM_APP + 3)
#define WM_SHOW_EXISTING (WM_APP + 4)

#define ID_TOGGLE 1001
#define ID_LOCATE 1002
#define ID_OPEN_LOG 1004
#define ID_PIN 1005
#define ID_CLOSE 1006

#define PATH_CAP 32768
#define STATUS_CAP 256
#define EVENT_LOG_MAX_LINES 400
#define LINE_CAP (256 * 1024)
#define LOG_PREFIX_CAP 4096
#define LIVE_TIMEOUT_MAX_AGE_MS UINT64_C(15000)
#define INVASION_TIMEOUT_MARKER "\"type\":\"timed_out\",\"context\":\"invasion\""

/*
 * The game narrates its own menu state.  Both edges of the Trial Board push
 * transition are logged, and input is refused for the whole transition, so the
 * requeue sequence follows those records instead of a fixed sleep.
 */
#define UI_BOARD_PUSH_MARKER "Menu page push transition starting: CharacterSheet_C"
#define UI_BOARD_POP_MARKER "Menu page pop transition starting: CharacterSheet_C"
#define UI_INPUTS_ENABLED_MARKER "Menu manager: enabling inputs"

#define ACTION_TAB_ACK_TIMEOUT_MS UINT64_C(3000)
#define ACTION_GATE_TIMEOUT_MS UINT64_C(6000)
#define ACTION_CONFIRM_SETTLE_MS UINT64_C(500)
/*
 * Tab is accepted while the game sits unfocused in the Sleep Room, verified by
 * posting one directly, but it is refused for some stretch after a matchmaking
 * timeout.  Nothing in the log says when that clears, and every ignored press
 * costs nothing, so the sequence keeps knocking for two minutes rather than
 * giving up and leaving the player out of the queue.
 */
#define SEARCH_RECOVERY_MAX_MS UINT64_C(6 * 60 * 60 * 1000)
#define ACTION_MAX_TAB_TRIES 40u
#define ACTION_NOTE_EVERY_TRIES 5u

/*
 * Posted mouse messages are dropped while the game believes it is in the
 * background.  Telling it otherwise with the same activation messages Windows
 * would send makes it process the click, and the reverse messages afterwards
 * put it back.  The operating system foreground window is never touched.
 */
#define CLICK_ACTIVATE_SETTLE_MS 250
#define CLICK_HOVER_MS 200
#define CLICK_HOLD_MS 100
#define CLICK_RELEASE_SETTLE_MS 700

/*
 * The START bar is a wide, light, neutral strip in the lower left of the Trial
 * Board.  It is located in a background capture of the game client instead of
 * being assumed at a fixed ratio, because the layout is anchored to the window
 * edges rather than scaled with it.
 */
#define START_REGION_RIGHT_PCT 45
#define START_REGION_TOP_PCT 70
#define START_BRIGHT_MIN 180
#define START_NEUTRAL_MAX 40
#define START_ROW_MIN_PCT 10
#define START_COLUMN_MIN_PCT 35
#define START_HEIGHT_MIN_PERMILLE 15
#define START_HEIGHT_MAX_PERMILLE 90
#define START_WIDTH_MIN_PCT 12
#define START_DENSITY_MIN_PCT 55
/* Measured on a 2560 by 1440 client; used only when no capture is possible. */
#define START_FALLBACK_X_NUM 461
#define START_FALLBACK_X_DEN 2560
#define START_FALLBACK_Y_NUM 1274
#define START_FALLBACK_Y_DEN 1440

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#define CLR_BG RGB(12, 12, 14)
#define CLR_PANEL RGB(20, 20, 24)
#define CLR_PANEL_HOT RGB(30, 30, 36)
#define CLR_PANEL_PRESS RGB(16, 16, 19)
#define CLR_LINE RGB(40, 40, 46)
#define CLR_TEXT RGB(236, 236, 240)
#define CLR_TEXT_DIM RGB(150, 152, 160)
#define CLR_TEXT_FAINT RGB(96, 98, 106)
#define CLR_ACCENT RGB(226, 48, 71)
#define CLR_ACCENT_HOT RGB(240, 70, 92)
#define CLR_ACCENT_PRESS RGB(170, 32, 50)

#define UI_W 320
#define UI_H_COMPACT 64
#define UI_H_EXPANDED 392
#define UI_PAD 14
#define UI_GLYPH_BUTTON 20
#define UI_ALPHA_IDLE 215
#define HOVER_COLLAPSE_MS 450
#define UI_CORNER_RADIUS 10
#define HIDE_COUNTDOWN_SECONDS 10

enum ui_font {
    F_TITLE,
    F_STATE,
    F_TIMER,
    F_STATUS,
    F_LABEL,
    F_VALUE,
    F_PATH,
    F_BTN_MAIN,
    F_BTN,
    F_LOG,
    F_SMALL,
    F_GLYPH,
    F_COUNT
};

enum ui_state {
    UI_STATE_OFF = 0,
    UI_STATE_ARMED,
    UI_STATE_SEARCHING,
    UI_STATE_REQUEUE,
    UI_STATE_DONE
};

enum start_source {
    START_SOURCE_NONE = 0,
    START_SOURCE_DETECTED,
    START_SOURCE_REMEMBERED,
    START_SOURCE_ESTIMATED
};

typedef struct file_identity {
    DWORD volume;
    DWORD index_high;
    DWORD index_low;
    BYTE prefix[LOG_PREFIX_CAP];
    DWORD prefix_length;
    bool valid;
} file_identity;

typedef struct start_button {
    int x;
    int y;
    int client_width;
    int client_height;
    int source;
} start_button;

typedef struct app_state {
    HINSTANCE instance;
    HWND window;
    HWND toggle_button;
    HWND locate_button;
    HWND log_button;
    HWND pin_button;
    HWND close_button;
    HWND event_log;
    HWND hot_button;
    bool expanded;
    bool pinned;
    bool hover_tracked;
    int expand_shift;
    /*
     * A found match ends the session: the widget counts down, then minimizes
     * and stops floating above other windows.  It floats only while armed.
     */
    bool hide_request;
    int hide_countdown;
    HICON icon;
    HFONT fonts[F_COUNT];
    HBRUSH background_brush;
    HBRUSH panel_brush;
    UINT ui_dpi;
    HANDLE instance_mutex;
    HANDLE worker;
    HANDLE stop_event;
    HANDLE wake_event;
    CRITICAL_SECTION lock;

    bool enabled;
    bool quitting;
    unsigned generation;
    rq_engine engine;
    uint64_t ui_board_push_ms;
    uint64_t ui_board_pop_ms;
    uint64_t ui_inputs_enabled_ms;
    int action_phase;
    int action_note;
    unsigned action_tab_tries;
    uint64_t action_tab_ms;
    uint64_t action_gate_ms;
    uint64_t display_total_ms;
    uint32_t display_requeue_count;
    /*
     * The search timer runs on the monotonic clock from the moment the
     * searching record is seen.  The record carries the matchmaking service
     * clock, and measuring against the local wall clock froze the timer at
     * zero for as long as the two disagreed.
     */
    bool search_live;
    uint64_t search_base_ms;
    uint64_t search_anchor_mono_ms;
    int ui_state;
    start_button start;

    bool display_enabled;
    int display_state;
    bool game_running;
    bool game_window_found;
    unsigned pulse;
    wchar_t disp_timer[32];
    wchar_t disp_status[STATUS_CAP];
    wchar_t disp_count[32];
    wchar_t disp_attempt[32];
    wchar_t disp_start[64];
    wchar_t status[STATUS_CAP];
    wchar_t game_dir[PATH_CAP];
    wchar_t log_path[PATH_CAP];
    wchar_t manifest_path[PATH_CAP];
    wchar_t settings_path[PATH_CAP];
    wchar_t module_dir[PATH_CAP];
} app_state;

static app_state g_app;

static void apply_topmost(bool topmost);

static uint64_t monotonic_ms(void)
{
    return (uint64_t)GetTickCount64();
}

static uint64_t wall_clock_ms(void)
{
    FILETIME file_time;
    ULARGE_INTEGER value;
    const uint64_t epoch_delta_100ns = UINT64_C(116444736000000000);

    GetSystemTimeAsFileTime(&file_time);
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (value.QuadPart < epoch_delta_100ns) {
        return 0;
    }
    return (value.QuadPart - epoch_delta_100ns) / UINT64_C(10000);
}

static uint64_t file_time_to_ms(const FILETIME *file_time)
{
    ULARGE_INTEGER value;

    value.LowPart = file_time->dwLowDateTime;
    value.HighPart = file_time->dwHighDateTime;
    return value.QuadPart / UINT64_C(10000);
}

static uint64_t local_clock_ms(void)
{
    SYSTEMTIME stamp;
    FILETIME file_time;

    GetLocalTime(&stamp);
    if (!SystemTimeToFileTime(&stamp, &file_time)) {
        return 0;
    }
    return file_time_to_ms(&file_time);
}

static bool read_fixed_digits(const char *text, size_t count, unsigned *output)
{
    unsigned value = 0;

    for (size_t index = 0; index < count; ++index) {
        if (!isdigit((unsigned char)text[index])) {
            return false;
        }
        value = value * 10u + (unsigned)(text[index] - '0');
    }
    *output = value;
    return true;
}

/* Read the engine's own "[2026.08.17-14.51.54:170]" prefix, in local time. */
static bool parse_log_local_ms(const char *line, uint64_t *output)
{
    SYSTEMTIME stamp;
    FILETIME file_time;
    unsigned year;
    unsigned month;
    unsigned day;
    unsigned hour;
    unsigned minute;
    unsigned second;
    unsigned millisecond;

    if (line == NULL || strlen(line) < 25 || line[0] != '[' ||
        line[5] != '.' || line[8] != '.' || line[11] != '-' ||
        line[14] != '.' || line[17] != '.' || line[20] != ':' ||
        line[24] != ']') {
        return false;
    }
    if (!read_fixed_digits(line + 1, 4, &year) ||
        !read_fixed_digits(line + 6, 2, &month) ||
        !read_fixed_digits(line + 9, 2, &day) ||
        !read_fixed_digits(line + 12, 2, &hour) ||
        !read_fixed_digits(line + 15, 2, &minute) ||
        !read_fixed_digits(line + 18, 2, &second) ||
        !read_fixed_digits(line + 21, 3, &millisecond)) {
        return false;
    }
    ZeroMemory(&stamp, sizeof(stamp));
    stamp.wYear = (WORD)year;
    stamp.wMonth = (WORD)month;
    stamp.wDay = (WORD)day;
    stamp.wHour = (WORD)hour;
    stamp.wMinute = (WORD)minute;
    stamp.wSecond = (WORD)second;
    stamp.wMilliseconds = (WORD)millisecond;
    if (!SystemTimeToFileTime(&stamp, &file_time)) {
        return false;
    }
    *output = file_time_to_ms(&file_time);
    return true;
}

static bool live_timeout_is_too_old(const char *line, uint64_t now_local_ms)
{
    uint64_t written_local_ms = 0;

    if (strstr(line, INVASION_TIMEOUT_MARKER) == NULL) {
        return false;
    }
    /*
     * The record carries the matchmaking service clock.  Comparing that with
     * the local clock disarms every single requeue on a machine whose time has
     * drifted a few seconds, which is a silent and very confusing failure.  The
     * game writes its own log prefix from this machine, so measuring against
     * that leaves no room for clock skew at all.
     */
    if (!parse_log_local_ms(line, &written_local_ms) || now_local_ms == 0 ||
        now_local_ms <= written_local_ms) {
        return false;
    }
    return now_local_ms - written_local_ms > LIVE_TIMEOUT_MAX_AGE_MS;
}

static bool file_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool directory_exists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool join_path(wchar_t *output,
                      size_t capacity,
                      const wchar_t *left,
                      const wchar_t *right)
{
    size_t length;
    const wchar_t *separator = L"\\";

    if (left == NULL || right == NULL || output == NULL || capacity == 0) {
        return false;
    }
    length = wcslen(left);
    if (length > 0 && (left[length - 1] == L'\\' || left[length - 1] == L'/')) {
        separator = L"";
    }
    return SUCCEEDED(StringCchPrintfW(
        output, capacity, L"%ls%ls%ls", left, separator, right));
}

static bool parent_path(wchar_t *path)
{
    wchar_t *slash;
    size_t length = wcslen(path);

    while (length > 0 && (path[length - 1] == L'\\' || path[length - 1] == L'/')) {
        path[--length] = L'\0';
    }
    slash = wcsrchr(path, L'\\');
    if (slash == NULL) {
        slash = wcsrchr(path, L'/');
    }
    if (slash == NULL) {
        return false;
    }
    *slash = L'\0';
    return true;
}

static void set_status_locked(const wchar_t *status)
{
    (void)StringCchCopyW(g_app.status, STATUS_CAP, status);
}

static void set_status(const wchar_t *status)
{
    EnterCriticalSection(&g_app.lock);
    set_status_locked(status);
    LeaveCriticalSection(&g_app.lock);
    if (g_app.window != NULL) {
        (void)PostMessageW(g_app.window, WM_STATE_CHANGED, 0, 0);
    }
}

static void post_ui_event(const wchar_t *format, ...)
{
    wchar_t *message = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                 512 * sizeof(wchar_t));
    va_list arguments;

    if (message == NULL) {
        return;
    }
    va_start(arguments, format);
    if (FAILED(StringCchVPrintfW(message, 512, format, arguments))) {
        (void)StringCchCopyW(message, 512, L"Event details were truncated.");
    }
    va_end(arguments);
    if (g_app.window == NULL ||
        !PostMessageW(g_app.window, WM_UI_EVENT, 0, (LPARAM)message)) {
        HeapFree(GetProcessHeap(), 0, message);
    }
}

static char *read_file_bytes(const wchar_t *path, DWORD *size_out)
{
    HANDLE file;
    LARGE_INTEGER size;
    char *buffer;
    DWORD read_count = 0;

    *size_out = 0;
    file = CreateFileW(path,
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return NULL;
    }
    buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                       (SIZE_T)size.QuadPart + 1);
    if (buffer == NULL) {
        CloseHandle(file);
        return NULL;
    }
    if (size.QuadPart > 0 &&
        (!ReadFile(file, buffer, (DWORD)size.QuadPart, &read_count, NULL) ||
         read_count != (DWORD)size.QuadPart)) {
        HeapFree(GetProcessHeap(), 0, buffer);
        CloseHandle(file);
        return NULL;
    }
    buffer[read_count] = '\0';
    *size_out = read_count;
    CloseHandle(file);
    return buffer;
}

static bool utf8_to_wide(const char *input, wchar_t *output, size_t capacity)
{
    int written;
    if (input == NULL || output == NULL || capacity == 0 || capacity > INT_MAX) {
        return false;
    }
    written = MultiByteToWideChar(
        CP_UTF8, 0, input, -1, output, (int)capacity);
    return written > 0;
}

static bool query_registry_string(HKEY root,
                                  const wchar_t *key_name,
                                  const wchar_t *value_name,
                                  REGSAM view,
                                  wchar_t *output,
                                  DWORD capacity)
{
    HKEY key;
    DWORD bytes = capacity * (DWORD)sizeof(wchar_t);
    DWORD type = 0;
    LSTATUS result;

    result = RegOpenKeyExW(root, key_name, 0, KEY_QUERY_VALUE | view, &key);
    if (result != ERROR_SUCCESS) {
        return false;
    }
    result = RegQueryValueExW(
        key, value_name, NULL, &type, (BYTE *)output, &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    output[(capacity - 1)] = L'\0';
    return true;
}

static bool game_dir_is_valid(const wchar_t *directory)
{
    wchar_t executable[PATH_CAP];
    return join_path(executable,
                     PATH_CAP,
                     directory,
                     L"OPP\\Binaries\\Win64\\" GAME_EXE) &&
           file_exists(executable);
}

static void update_derived_paths(void)
{
    wchar_t steamapps[PATH_CAP];

    (void)StringCchCopyW(steamapps, PATH_CAP, g_app.game_dir);
    if (parent_path(steamapps) && parent_path(steamapps)) {
        (void)join_path(g_app.manifest_path,
                        PATH_CAP,
                        steamapps,
                        L"appmanifest_" STEAM_APP_ID L".acf");
    } else {
        g_app.manifest_path[0] = L'\0';
    }
}

static bool try_library_root(const wchar_t *library_root)
{
    wchar_t candidate[PATH_CAP];

    if (!join_path(candidate,
                   PATH_CAP,
                   library_root,
                   L"steamapps\\common\\The Outlast Trials")) {
        return false;
    }
    if (!game_dir_is_valid(candidate)) {
        return false;
    }
    (void)StringCchCopyW(g_app.game_dir, PATH_CAP, candidate);
    update_derived_paths();
    return true;
}

static bool parse_libraryfolders(const wchar_t *steam_root)
{
    wchar_t vdf_path[PATH_CAP];
    char *contents;
    char *cursor;
    DWORD size;

    if (!join_path(vdf_path,
                   PATH_CAP,
                   steam_root,
                   L"steamapps\\libraryfolders.vdf")) {
        return false;
    }
    contents = read_file_bytes(vdf_path, &size);
    if (contents == NULL) {
        return false;
    }
    (void)size;
    cursor = contents;
    while ((cursor = strstr(cursor, "\"path\"")) != NULL) {
        char raw[PATH_CAP];
        char unescaped[PATH_CAP];
        wchar_t wide[PATH_CAP];
        char *begin;
        char *end;
        size_t in_index = 0;
        size_t out_index = 0;

        begin = cursor + strlen("\"path\"");
        while (*begin != '\0' && *begin != '"') {
            ++begin;
        }
        if (*begin == '\0') {
            break;
        }
        ++begin;
        end = strchr(begin, '"');
        if (end == NULL) {
            break;
        }
        if ((size_t)(end - begin) >= sizeof(raw)) {
            cursor = end + 1;
            continue;
        }
        memcpy(raw, begin, (size_t)(end - begin));
        raw[end - begin] = '\0';
        while (raw[in_index] != '\0' && out_index + 1 < sizeof(unescaped)) {
            if (raw[in_index] == '\\' && raw[in_index + 1] == '\\') {
                ++in_index;
            }
            unescaped[out_index++] = raw[in_index++];
        }
        unescaped[out_index] = '\0';
        if (utf8_to_wide(unescaped, wide, PATH_CAP) && try_library_root(wide)) {
            HeapFree(GetProcessHeap(), 0, contents);
            return true;
        }
        cursor = end + 1;
    }
    HeapFree(GetProcessHeap(), 0, contents);
    return false;
}

static bool discover_game_dir(void)
{
    wchar_t saved[PATH_CAP];
    wchar_t steam[PATH_CAP];
    wchar_t environment[PATH_CAP];
    DWORD length;

    length = GetEnvironmentVariableW(
        L"OUTLAST_REQUEUE_GAME_DIR", environment, PATH_CAP);
    if (length > 0 && length < PATH_CAP && game_dir_is_valid(environment)) {
        (void)StringCchCopyW(g_app.game_dir, PATH_CAP, environment);
        update_derived_paths();
        return true;
    }

    saved[0] = L'\0';
    GetPrivateProfileStringW(L"Paths",
                             L"GameDir",
                             L"",
                             saved,
                             PATH_CAP,
                             g_app.settings_path);
    if (game_dir_is_valid(saved)) {
        (void)StringCchCopyW(g_app.game_dir, PATH_CAP, saved);
        update_derived_paths();
        return true;
    }

    if (query_registry_string(HKEY_CURRENT_USER,
                              L"Software\\Valve\\Steam",
                              L"SteamPath",
                              0,
                              steam,
                              PATH_CAP) &&
        (try_library_root(steam) || parse_libraryfolders(steam))) {
        return true;
    }
    if (query_registry_string(HKEY_LOCAL_MACHINE,
                              L"Software\\Valve\\Steam",
                              L"InstallPath",
                              KEY_WOW64_32KEY,
                              steam,
                              PATH_CAP) &&
        (try_library_root(steam) || parse_libraryfolders(steam))) {
        return true;
    }
    if (try_library_root(L"C:\\Program Files (x86)\\Steam") ||
        try_library_root(L"C:\\Program Files\\Steam")) {
        return true;
    }
    g_app.game_dir[0] = L'\0';
    g_app.manifest_path[0] = L'\0';
    return false;
}

static void load_remembered_start(void)
{
    start_button remembered;

    ZeroMemory(&remembered, sizeof(remembered));
    remembered.x = (int)GetPrivateProfileIntW(
        L"StartButton", L"X", 0, g_app.settings_path);
    remembered.y = (int)GetPrivateProfileIntW(
        L"StartButton", L"Y", 0, g_app.settings_path);
    remembered.client_width = (int)GetPrivateProfileIntW(
        L"StartButton", L"ClientWidth", 0, g_app.settings_path);
    remembered.client_height = (int)GetPrivateProfileIntW(
        L"StartButton", L"ClientHeight", 0, g_app.settings_path);
    if (remembered.x > 0 && remembered.y > 0 && remembered.client_width > 0 &&
        remembered.client_height > 0) {
        remembered.source = START_SOURCE_REMEMBERED;
        g_app.start = remembered;
    }
}

static void save_remembered_start(const start_button *button)
{
    wchar_t value[32];
    const struct {
        const wchar_t *key;
        int number;
    } entries[] = {
        {L"X", button->x},
        {L"Y", button->y},
        {L"ClientWidth", button->client_width},
        {L"ClientHeight", button->client_height},
    };

    for (size_t index = 0; index < ARRAYSIZE(entries); ++index) {
        (void)StringCchPrintfW(
            value, ARRAYSIZE(value), L"%d", entries[index].number);
        (void)WritePrivateProfileStringW(
            L"StartButton", entries[index].key, value, g_app.settings_path);
    }
}

static bool initialize_paths(void)
{
    PWSTR local_app_data = NULL;
    wchar_t config_dir[PATH_CAP];
    wchar_t module[PATH_CAP];

    if (FAILED(SHGetKnownFolderPath(
            &FOLDERID_LocalAppData, KF_FLAG_DEFAULT, NULL, &local_app_data))) {
        return false;
    }
    (void)join_path(g_app.log_path,
                    PATH_CAP,
                    local_app_data,
                    L"OPP\\Saved\\Logs\\OPP.log");
    (void)join_path(config_dir,
                    PATH_CAP,
                    local_app_data,
                    L"OutlastRequeue");
    if (!directory_exists(config_dir)) {
        (void)CreateDirectoryW(config_dir, NULL);
    }
    (void)join_path(g_app.settings_path,
                    PATH_CAP,
                    config_dir,
                    L"settings.ini");
    CoTaskMemFree(local_app_data);

    if (GetModuleFileNameW(NULL, module, PATH_CAP) == 0) {
        return false;
    }
    if (!parent_path(module)) {
        return false;
    }
    (void)StringCchCopyW(g_app.module_dir, PATH_CAP, module);
    (void)discover_game_dir();
    load_remembered_start();
    return true;
}

static bool parse_build_id(wchar_t *output, size_t capacity)
{
    DWORD size;
    char *contents = read_file_bytes(g_app.manifest_path, &size);
    char *marker;
    char *begin;
    char *end;
    char value[64];

    (void)size;
    if (contents == NULL) {
        return false;
    }
    marker = strstr(contents, "\"buildid\"");
    if (marker == NULL) {
        HeapFree(GetProcessHeap(), 0, contents);
        return false;
    }
    begin = marker + strlen("\"buildid\"");
    while (*begin != '\0' && *begin != '"') {
        ++begin;
    }
    if (*begin == '\0') {
        HeapFree(GetProcessHeap(), 0, contents);
        return false;
    }
    ++begin;
    end = strchr(begin, '"');
    if (end == NULL || end == begin || (size_t)(end - begin) >= sizeof(value)) {
        HeapFree(GetProcessHeap(), 0, contents);
        return false;
    }
    memcpy(value, begin, (size_t)(end - begin));
    value[end - begin] = '\0';
    HeapFree(GetProcessHeap(), 0, contents);
    return utf8_to_wide(value, output, capacity);
}

static bool validate_install(wchar_t *reason, size_t capacity)
{
    if (!game_dir_is_valid(g_app.game_dir)) {
        (void)StringCchCopyW(
            reason, capacity, L"The Outlast Trials folder was not found.");
        return false;
    }
    if (!file_exists(g_app.log_path)) {
        (void)StringCchCopyW(
            reason,
            capacity,
            L"OPP.log was not found. Start the game once, then try again.");
        return false;
    }
    reason[0] = L'\0';
    return true;
}

static bool normalized_path_equals(const wchar_t *left, const wchar_t *right)
{
    wchar_t left_full[PATH_CAP];
    wchar_t right_full[PATH_CAP];
    DWORD left_length;
    DWORD right_length;
    wchar_t *left_value = left_full;
    wchar_t *right_value = right_full;

    left_length = GetFullPathNameW(left, PATH_CAP, left_full, NULL);
    right_length = GetFullPathNameW(right, PATH_CAP, right_full, NULL);
    if (left_length == 0 || left_length >= PATH_CAP || right_length == 0 ||
        right_length >= PATH_CAP) {
        return false;
    }
    if (wcsncmp(left_value, L"\\\\?\\", 4) == 0) {
        left_value += 4;
    }
    if (wcsncmp(right_value, L"\\\\?\\", 4) == 0) {
        right_value += 4;
    }
    for (wchar_t *cursor = left_value; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'/') {
            *cursor = L'\\';
        }
    }
    for (wchar_t *cursor = right_value; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'/') {
            *cursor = L'\\';
        }
    }
    return _wcsicmp(left_value, right_value) == 0;
}

static bool paths_refer_to_same_file(const wchar_t *left, const wchar_t *right)
{
    HANDLE left_handle;
    HANDLE right_handle;
    BY_HANDLE_FILE_INFORMATION left_info;
    BY_HANDLE_FILE_INFORMATION right_info;
    bool matches = false;
    const DWORD sharing =
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    left_handle = CreateFileW(left,
                              FILE_READ_ATTRIBUTES,
                              sharing,
                              NULL,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);
    if (left_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    right_handle = CreateFileW(right,
                               FILE_READ_ATTRIBUTES,
                               sharing,
                               NULL,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               NULL);
    if (right_handle != INVALID_HANDLE_VALUE) {
        if (GetFileInformationByHandle(left_handle, &left_info) &&
            GetFileInformationByHandle(right_handle, &right_info)) {
            matches =
                left_info.dwVolumeSerialNumber ==
                    right_info.dwVolumeSerialNumber &&
                left_info.nFileIndexHigh == right_info.nFileIndexHigh &&
                left_info.nFileIndexLow == right_info.nFileIndexLow;
        }
        CloseHandle(right_handle);
    }
    CloseHandle(left_handle);
    return matches;
}

static bool process_matches_discovered_game(DWORD process_id)
{
    HANDLE process;
    wchar_t image[PATH_CAP];
    wchar_t expected[PATH_CAP];
    DWORD capacity = PATH_CAP;
    bool matches = false;

    if (g_app.game_dir[0] == L'\0' ||
        !join_path(expected,
                   PATH_CAP,
                   g_app.game_dir,
                   L"OPP\\Binaries\\Win64\\" GAME_EXE)) {
        return false;
    }
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == NULL) {
        return false;
    }
    image[0] = L'\0';
    if (QueryFullProcessImageNameW(process, 0, image, &capacity)) {
        matches = normalized_path_equals(image, expected) ||
                  paths_refer_to_same_file(image, expected);
    }
    CloseHandle(process);
    return matches;
}

static DWORD find_game_process(void)
{
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    DWORD process_id = 0;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, GAME_EXE) == 0 &&
                process_matches_discovered_game(entry.th32ProcessID)) {
                process_id = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return process_id;
}

typedef struct window_search {
    HWND result;
} window_search;

static BOOL CALLBACK find_game_window_callback(HWND window, LPARAM parameter)
{
    window_search *search = (window_search *)parameter;
    DWORD process_id = 0;
    wchar_t class_name[128];

    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != NULL ||
        GetAncestor(window, GA_ROOT) != window) {
        return TRUE;
    }
    class_name[0] = L'\0';
    (void)GetClassNameW(window, class_name, ARRAYSIZE(class_name));
    if (_wcsicmp(class_name, L"UnrealWindow") != 0) {
        return TRUE;
    }
    (void)GetWindowThreadProcessId(window, &process_id);
    if (process_id != 0 && process_matches_discovered_game(process_id)) {
        search->result = window;
        return FALSE;
    }
    return TRUE;
}

static HWND find_game_window(void)
{
    window_search search;
    search.result = NULL;
    (void)EnumWindows(find_game_window_callback, (LPARAM)&search);
    return search.result;
}

static bool generation_is_enabled(unsigned generation)
{
    bool enabled;
    EnterCriticalSection(&g_app.lock);
    enabled = g_app.enabled && g_app.generation == generation;
    LeaveCriticalSection(&g_app.lock);
    return enabled;
}

/* Sleep on the worker without ignoring a shutdown request. */
static void worker_pause(DWORD milliseconds)
{
    (void)WaitForSingleObject(g_app.stop_event, milliseconds);
}

static bool post_targeted_key(HWND game_window,
                              UINT virtual_key,
                              unsigned generation)
{
    UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    LPARAM down = (LPARAM)1 | ((LPARAM)scan_code << 16);
    LPARAM up = down | ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    bool result = false;

    /*
     * Serialize the final generation check with the targeted key pair.  Once
     * OFF commits under this lock, no later Tab can be posted by an older
     * action generation.  Holding it for the 80 ms pair also guarantees key-up.
     */
    EnterCriticalSection(&g_app.lock);
    if (!g_app.enabled || g_app.generation != generation) {
        LeaveCriticalSection(&g_app.lock);
        return false;
    }
    if (PostMessageW(game_window, WM_KEYDOWN, virtual_key, down)) {
        worker_pause(80);
        result = PostMessageW(game_window, WM_KEYUP, virtual_key, up) != FALSE;
    }
    LeaveCriticalSection(&g_app.lock);
    return result;
}

/*
 * Click a client-area point of the game window from the background.  The
 * game only routes mouse messages while it believes it is the active
 * application, so it is told that it is, by message, for the duration of the
 * click, and told the opposite afterwards.  Nothing here changes which window
 * the operating system considers foreground, and the pointer never moves.
 */
static bool post_targeted_click(HWND game_window,
                                int x,
                                int y,
                                unsigned generation)
{
    LPARAM position = MAKELPARAM(x, y);
    bool clicked = false;

    if (!generation_is_enabled(generation)) {
        return false;
    }
    (void)PostMessageW(game_window, WM_ACTIVATEAPP, TRUE, 0);
    (void)PostMessageW(game_window, WM_ACTIVATE, WA_ACTIVE, 0);
    (void)PostMessageW(game_window, WM_SETFOCUS, 0, 0);
    worker_pause(CLICK_ACTIVATE_SETTLE_MS);
    if (generation_is_enabled(generation)) {
        (void)PostMessageW(game_window, WM_MOUSEMOVE, 0, position);
        worker_pause(CLICK_HOVER_MS);
        if (PostMessageW(game_window, WM_LBUTTONDOWN, MK_LBUTTON, position)) {
            worker_pause(CLICK_HOLD_MS);
            clicked = PostMessageW(game_window, WM_LBUTTONUP, 0, position) !=
                      FALSE;
        }
        worker_pause(CLICK_RELEASE_SETTLE_MS);
    }
    (void)PostMessageW(game_window, WM_KILLFOCUS, 0, 0);
    (void)PostMessageW(game_window, WM_ACTIVATE, WA_INACTIVE, 0);
    (void)PostMessageW(game_window, WM_ACTIVATEAPP, FALSE, 0);
    return clicked;
}

typedef struct client_capture {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ previous;
    BYTE *pixels;
    int width;
    int height;
} client_capture;

static void release_capture(client_capture *capture)
{
    if (capture->dc != NULL && capture->previous != NULL) {
        SelectObject(capture->dc, capture->previous);
    }
    if (capture->bitmap != NULL) {
        DeleteObject(capture->bitmap);
    }
    if (capture->dc != NULL) {
        DeleteDC(capture->dc);
    }
    ZeroMemory(capture, sizeof(*capture));
}

/* Render the game client area into a top-down 32-bit buffer without showing it. */
static bool capture_game_client(HWND game_window, client_capture *capture)
{
    RECT client;
    BITMAPINFO info;
    void *bits = NULL;

    ZeroMemory(capture, sizeof(*capture));
    if (!GetClientRect(game_window, &client) || client.right <= 0 ||
        client.bottom <= 0 || client.right > 16384 || client.bottom > 16384) {
        return false;
    }
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    capture->dc = CreateCompatibleDC(NULL);
    if (capture->dc == NULL) {
        return false;
    }
    capture->bitmap =
        CreateDIBSection(capture->dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (capture->bitmap == NULL || bits == NULL) {
        release_capture(capture);
        return false;
    }
    capture->previous = SelectObject(capture->dc, capture->bitmap);
    if (!PrintWindow(game_window, capture->dc, PW_RENDERFULLCONTENT)) {
        release_capture(capture);
        return false;
    }
    GdiFlush();
    capture->pixels = bits;
    capture->width = client.right;
    capture->height = client.bottom;
    return true;
}

static bool pixel_is_bright(const BYTE *pixel)
{
    BYTE low = pixel[0];
    BYTE high = pixel[0];

    for (int channel = 1; channel < 3; ++channel) {
        if (pixel[channel] < low) {
            low = pixel[channel];
        }
        if (pixel[channel] > high) {
            high = pixel[channel];
        }
    }
    return low >= START_BRIGHT_MIN && (high - low) <= START_NEUTRAL_MAX;
}

/*
 * Locate the START bar in a top-down BGRA client capture.  A band of rows in
 * the lower-left region that is mostly light and neutral is the bar; the
 * lettering inside it is dark, so the search tolerates gaps in both axes.
 */
static bool find_start_button(const BYTE *pixels,
                              int width,
                              int height,
                              POINT *center)
{
    const int region_right = width * START_REGION_RIGHT_PCT / 100;
    const int region_top = height * START_REGION_TOP_PCT / 100;
    const int row_minimum = width * START_ROW_MIN_PCT / 100;
    const int height_minimum = height * START_HEIGHT_MIN_PERMILLE / 1000;
    const int height_maximum = height * START_HEIGHT_MAX_PERMILLE / 1000;
    const int width_minimum = width * START_WIDTH_MIN_PCT / 100;
    const size_t stride = (size_t)width * 4;
    int best_top = -1;
    int best_bottom = -1;
    int band_top = -1;
    int gap = 0;
    int column_minimum;
    int best_left = -1;
    int best_right = -1;
    int run_left = -1;
    int run_gap = 0;

    if (pixels == NULL || width < 64 || height < 64 || region_right <= 0 ||
        region_top >= height) {
        return false;
    }
    for (int y = region_top; y <= height; ++y) {
        int count = 0;
        bool bar_row;

        if (y < height) {
            const BYTE *row = pixels + (size_t)y * stride;
            for (int x = 0; x < region_right; ++x) {
                if (pixel_is_bright(row + (size_t)x * 4)) {
                    ++count;
                }
            }
        }
        bar_row = y < height && count >= row_minimum;
        if (bar_row) {
            if (band_top < 0) {
                band_top = y;
            }
            gap = 0;
            continue;
        }
        if (band_top >= 0 && ++gap > 1) {
            int band_bottom = y - gap;
            int band_height = band_bottom - band_top + 1;
            if (band_height >= height_minimum &&
                band_height <= height_maximum &&
                band_height > best_bottom - best_top) {
                best_top = band_top;
                best_bottom = band_bottom;
            }
            band_top = -1;
            gap = 0;
        }
    }
    if (best_top < 0) {
        return false;
    }
    column_minimum = (best_bottom - best_top + 1) * START_COLUMN_MIN_PCT / 100;
    for (int x = 0; x <= region_right; ++x) {
        int count = 0;
        bool bar_column;

        if (x < region_right) {
            for (int y = best_top; y <= best_bottom; ++y) {
                if (pixel_is_bright(pixels + (size_t)y * stride +
                                    (size_t)x * 4)) {
                    ++count;
                }
            }
        }
        bar_column = x < region_right && count >= column_minimum;
        if (bar_column) {
            if (run_left < 0) {
                run_left = x;
            }
            run_gap = 0;
            continue;
        }
        if (run_left >= 0 && ++run_gap > 2) {
            int run_right = x - run_gap;
            if (run_right - run_left > best_right - best_left) {
                best_left = run_left;
                best_right = run_right;
            }
            run_left = -1;
            run_gap = 0;
        }
    }
    if (best_left < 0 || best_right - best_left + 1 < width_minimum) {
        return false;
    }
    /*
     * Lettering is sparse and a bar is solid.  A line of white text can pass
     * the row and column tests on a large client, so the winning rectangle
     * must also be mostly bright.
     */
    {
        int bright = 0;
        int area = (best_right - best_left + 1) * (best_bottom - best_top + 1);
        for (int y = best_top; y <= best_bottom; ++y) {
            const BYTE *row = pixels + (size_t)y * stride;
            for (int x = best_left; x <= best_right; ++x) {
                if (pixel_is_bright(row + (size_t)x * 4)) {
                    ++bright;
                }
            }
        }
        if (area <= 0 || bright * 100 < area * START_DENSITY_MIN_PCT) {
            return false;
        }
    }
    center->x = (best_left + best_right) / 2;
    center->y = (best_top + best_bottom) / 2;
    return true;
}

/*
 * Decide where START is for this click.  A fresh capture wins; otherwise the
 * last detected position is reused when the client size still matches, and as
 * a last resort the position measured on the reference client is scaled.
 */
static bool resolve_start_button(HWND game_window, start_button *button)
{
    client_capture capture;
    POINT center;
    RECT client;
    start_button remembered;

    ZeroMemory(button, sizeof(*button));
    if (capture_game_client(game_window, &capture)) {
        bool found = find_start_button(
            capture.pixels, capture.width, capture.height, &center);
        button->client_width = capture.width;
        button->client_height = capture.height;
        release_capture(&capture);
        if (found) {
            button->x = center.x;
            button->y = center.y;
            button->source = START_SOURCE_DETECTED;
            return true;
        }
    } else if (GetClientRect(game_window, &client)) {
        button->client_width = client.right;
        button->client_height = client.bottom;
    }
    if (button->client_width <= 0 || button->client_height <= 0) {
        return false;
    }
    EnterCriticalSection(&g_app.lock);
    remembered = g_app.start;
    LeaveCriticalSection(&g_app.lock);
    if (remembered.source != START_SOURCE_NONE &&
        remembered.source != START_SOURCE_ESTIMATED &&
        remembered.client_width == button->client_width &&
        remembered.client_height == button->client_height) {
        button->x = remembered.x;
        button->y = remembered.y;
        button->source = START_SOURCE_REMEMBERED;
        return true;
    }
    button->x = MulDiv(button->client_width,
                       START_FALLBACK_X_NUM,
                       START_FALLBACK_X_DEN);
    button->y = MulDiv(button->client_height,
                       START_FALLBACK_Y_NUM,
                       START_FALLBACK_Y_DEN);
    button->source = START_SOURCE_ESTIMATED;
    return true;
}

static const wchar_t *start_source_name(int source)
{
    switch (source) {
    case START_SOURCE_DETECTED:
        return L"detected in a background capture";
    case START_SOURCE_REMEMBERED:
        return L"reused from the last detection";
    case START_SOURCE_ESTIMATED:
        return L"estimated from the reference layout";
    default:
        return L"unknown";
    }
}

/* Locate START on the open Trial Board and click it without activating the game. */
static bool click_start_button(unsigned generation)
{
    HWND game_window = find_game_window();
    start_button button;

    if (game_window == NULL) {
        return false;
    }
    if (!resolve_start_button(game_window, &button)) {
        post_ui_event(L"The game client area could not be measured.");
        return false;
    }
    EnterCriticalSection(&g_app.lock);
    g_app.start = button;
    LeaveCriticalSection(&g_app.lock);
    if (button.source == START_SOURCE_DETECTED) {
        save_remembered_start(&button);
    } else {
        post_ui_event(L"START was not visible in the capture; "
                      L"position %ls.",
                      start_source_name(button.source));
    }
    post_ui_event(L"Clicking START at %d, %d in a %d by %d client (%ls).",
                  button.x,
                  button.y,
                  button.client_width,
                  button.client_height,
                  start_source_name(button.source));
    return post_targeted_click(game_window, button.x, button.y, generation);
}

/*
 * The Trial Board refuses input for the whole push transition, measured at
 * 1.26 to 1.32 seconds on a live client.  The sequence below is driven by the
 * monitor loop: it posts Tab, waits for the game to log that the board is
 * accepting input again, and only then clicks START.  It never blocks the
 * loop, because the loop is also what feeds it those records.
 */
enum action_phase {
    ACTION_IDLE = 0,
    ACTION_AWAIT_BOARD,
    ACTION_AWAIT_GATE,
    ACTION_SETTLE
};

enum action_step {
    STEP_NONE = 0,
    STEP_TAB,
    STEP_CONFIRM,
    STEP_ABANDON
};

enum action_note {
    NOTE_NONE = 0,
    NOTE_REOPEN,
    NOTE_TAB_RETRY,
    NOTE_GATE_ASSUMED
};

static bool post_key_to_game(UINT virtual_key, unsigned generation)
{
    HWND game_window = find_game_window();

    if (game_window == NULL) {
        return false;
    }
    return post_targeted_key(game_window, virtual_key, generation);
}

static void reset_action_sequence_locked(void)
{
    g_app.action_phase = ACTION_IDLE;
    g_app.action_note = NOTE_NONE;
    g_app.action_tab_tries = 0;
    g_app.action_tab_ms = 0;
    g_app.action_gate_ms = 0;
}

static int advance_action_sequence_locked(uint64_t now_ms)
{
    switch (g_app.action_phase) {
    case ACTION_AWAIT_BOARD:
        if (g_app.ui_board_push_ms > g_app.action_tab_ms) {
            g_app.action_phase = ACTION_AWAIT_GATE;
            return STEP_NONE;
        }
        /*
         * Tab is a toggle.  A pop rather than a push means the board was
         * already open and the press closed it, so it has to be reopened
         * before START exists to click.
         */
        if (g_app.ui_board_pop_ms > g_app.action_tab_ms ||
            now_ms - g_app.action_tab_ms > ACTION_TAB_ACK_TIMEOUT_MS) {
            if (g_app.action_tab_tries >= ACTION_MAX_TAB_TRIES) {
                return STEP_ABANDON;
            }
            if (g_app.ui_board_pop_ms > g_app.action_tab_ms) {
                g_app.action_note = NOTE_REOPEN;
            } else if (g_app.action_tab_tries % ACTION_NOTE_EVERY_TRIES == 0) {
                g_app.action_note = NOTE_TAB_RETRY;
            }
            return STEP_TAB;
        }
        return STEP_NONE;
    case ACTION_AWAIT_GATE:
        if (g_app.ui_inputs_enabled_ms > g_app.ui_board_push_ms) {
            g_app.action_gate_ms = now_ms;
            g_app.action_phase = ACTION_SETTLE;
            return STEP_NONE;
        }
        if (now_ms - g_app.action_tab_ms > ACTION_GATE_TIMEOUT_MS) {
            g_app.action_note = NOTE_GATE_ASSUMED;
            g_app.action_gate_ms = now_ms;
            g_app.action_phase = ACTION_SETTLE;
        }
        return STEP_NONE;
    case ACTION_SETTLE:
        if (now_ms - g_app.action_gate_ms >= ACTION_CONFIRM_SETTLE_MS) {
            return STEP_CONFIRM;
        }
        return STEP_NONE;
    default:
        return STEP_NONE;
    }
}

static void post_tab_failure_note(void)
{
    HWND game_window = find_game_window();
    wchar_t message[STATUS_CAP];

    if (game_window == NULL) {
        post_ui_event(L"The exact Outlast window disappeared mid-sequence.");
        return;
    }
    if (SUCCEEDED(StringCchPrintfW(
            message,
            ARRAYSIZE(message),
            L"Tab ignored for %u attempts over two minutes "
            L"(window %ls and %ls).",
            ACTION_MAX_TAB_TRIES,
            IsIconic(game_window) ? L"minimized" : L"restored",
            GetForegroundWindow() == game_window ? L"focused" : L"unfocused"))) {
        post_ui_event(message);
    }
}

static void post_action_note(int note)
{
    switch (note) {
    case NOTE_REOPEN:
        post_ui_event(L"Trial Board was already open; reopening it.");
        break;
    case NOTE_TAB_RETRY:
        post_ui_event(L"Tab still not acknowledged; the game refuses it for "
                      L"now, still trying.");
        break;
    case NOTE_GATE_ASSUMED:
        post_ui_event(L"Board opened but never reported ready; "
                      L"clicking START anyway.");
        break;
    default:
        break;
    }
}

static bool get_file_identity(HANDLE file, file_identity *identity)
{
    BY_HANDLE_FILE_INFORMATION information;
    LARGE_INTEGER beginning;
    LARGE_INTEGER size;
    DWORD requested;
    DWORD read = 0;

    ZeroMemory(identity, sizeof(*identity));
    if (!GetFileInformationByHandle(file, &information)) {
        return false;
    }
    identity->volume = information.dwVolumeSerialNumber;
    identity->index_high = information.nFileIndexHigh;
    identity->index_low = information.nFileIndexLow;
    beginning.QuadPart = 0;
    size.QuadPart = 0;
    if (!GetFileSizeEx(file, &size) ||
        !SetFilePointerEx(file, beginning, NULL, FILE_BEGIN)) {
        return false;
    }
    requested = size.QuadPart > LOG_PREFIX_CAP
                    ? LOG_PREFIX_CAP
                    : (DWORD)(size.QuadPart > 0 ? size.QuadPart : 0);
    if (requested > 0 &&
        (!ReadFile(file, identity->prefix, requested, &read, NULL) ||
         read != requested)) {
        return false;
    }
    identity->prefix_length = read;
    identity->valid = true;
    return true;
}

static bool same_identity(const file_identity *left, const file_identity *right)
{
    return left->valid && right->valid && left->volume == right->volume &&
           left->index_high == right->index_high &&
           left->index_low == right->index_low &&
           right->prefix_length >= left->prefix_length &&
           memcmp(left->prefix, right->prefix, left->prefix_length) == 0;
}

static void apply_engine_event_locked(const rq_event *event)
{
    if (event->type != RQ_EVENT_NONE) {
        g_app.display_requeue_count = event->requeue_count;
    }
    switch (event->type) {
    case RQ_EVENT_SEARCHING: {
        uint64_t now_mono = monotonic_ms();
        uint64_t already_ms = 0;

        if (!event->requeue_confirmed && !event->recovered) {
            g_app.display_total_ms = 0;
        }
        if (event->recovered && event->has_started_at) {
            uint64_t now_wall = wall_clock_ms();
            if (now_wall > event->started_at_ms) {
                already_ms = now_wall - event->started_at_ms;
            }
            if (already_ms > SEARCH_RECOVERY_MAX_MS) {
                already_ms = SEARCH_RECOVERY_MAX_MS;
            }
        }
        g_app.search_live = true;
        g_app.search_base_ms = event->cumulative_before_ms;
        g_app.search_anchor_mono_ms = now_mono - already_ms;
        g_app.ui_state = UI_STATE_SEARCHING;
        set_status_locked(L"Searching for an Invasion match");
        break;
    }
    case RQ_EVENT_TIMEOUT:
        g_app.search_live = false;
        g_app.display_total_ms = event->total_search_ms;
        g_app.ui_state = UI_STATE_REQUEUE;
        set_status_locked(L"Timeout detected, letting the client settle");
        break;
    case RQ_EVENT_SUCCEEDED:
        g_app.search_live = false;
        g_app.display_total_ms = event->total_search_ms;
        g_app.enabled = false;
        ++g_app.generation;
        g_app.hide_request = true;
        g_app.ui_state = UI_STATE_DONE;
        set_status_locked(L"Match found, automation stopped");
        break;
    case RQ_EVENT_CANCELED:
        g_app.search_live = false;
        g_app.display_total_ms = event->total_search_ms;
        g_app.ui_state = UI_STATE_ARMED;
        set_status_locked(L"Search canceled, waiting for a manual search");
        break;
    case RQ_EVENT_DISCONNECTED:
        g_app.search_live = false;
        g_app.display_total_ms = event->total_search_ms;
        g_app.ui_state = UI_STATE_ARMED;
        set_status_locked(L"Server disconnected, waiting for a manual search");
        break;
    case RQ_EVENT_REQUEUE_POSTED:
        g_app.ui_state = UI_STATE_REQUEUE;
        set_status_locked(L"START clicked, waiting for a new ticket");
        break;
    case RQ_EVENT_REQUEUE_RETRY:
        g_app.ui_state = UI_STATE_REQUEUE;
        set_status_locked(L"No ticket yet, preparing another attempt");
        break;
    case RQ_EVENT_REQUEUE_UNCONFIRMED:
        g_app.ui_state = UI_STATE_ARMED;
        set_status_locked(L"Requeue not confirmed, start a search manually");
        break;
    case RQ_EVENT_REQUEUE_EXPIRED:
        g_app.ui_state = UI_STATE_ARMED;
        set_status_locked(L"Timing window expired, no input was sent");
        break;
    case RQ_EVENT_HELPER_FAILED:
        g_app.ui_state = UI_STATE_ARMED;
        set_status_locked(L"Game window unavailable, waiting");
        break;
    case RQ_EVENT_REQUEUE_DUE:
        g_app.ui_state = UI_STATE_REQUEUE;
        set_status_locked(L"Opening the Trial Board");
        break;
    case RQ_EVENT_NONE:
    default:
        break;
    }
}

static void describe_engine_event(const rq_event *event)
{
    switch (event->type) {
    case RQ_EVENT_SEARCHING:
        post_ui_event(event->recovered
                          ? L"Recovered an active Invasion search."
                          : (event->requeue_confirmed
                                 ? L"Requeue confirmed by a new Invasion ticket."
                                 : L"Invasion search detected."));
        break;
    case RQ_EVENT_TIMEOUT:
        post_ui_event(L"The armed Invasion ticket timed out.");
        break;
    case RQ_EVENT_SUCCEEDED:
        post_ui_event(L"Invasion match found; automation stopped.");
        break;
    case RQ_EVENT_CANCELED:
        post_ui_event(L"Invasion search canceled; nothing armed.");
        break;
    case RQ_EVENT_DISCONNECTED:
        post_ui_event(L"Game server connection lost; no input sent.");
        break;
    case RQ_EVENT_REQUEUE_POSTED:
        post_ui_event(L"Tab and START posted without activating the game.");
        break;
    case RQ_EVENT_REQUEUE_RETRY:
        post_ui_event(L"No replacement ticket in time; repeating the sequence.");
        break;
    case RQ_EVENT_REQUEUE_UNCONFIRMED:
        post_ui_event(L"No new Invasion ticket after the final attempt.");
        break;
    case RQ_EVENT_REQUEUE_EXPIRED:
        post_ui_event(L"The safe action window elapsed; no delayed input was sent.");
        break;
    case RQ_EVENT_HELPER_FAILED:
        post_ui_event(L"Could not reach the exact Outlast process window.");
        break;
    default:
        break;
    }
}

static void process_log_line(const char *line,
                             bool initial_scan,
                             unsigned generation)
{
    rq_event event;
    uint64_t now_wall_ms = wall_clock_ms();
    EnterCriticalSection(&g_app.lock);
    if (!initial_scan) {
        uint64_t seen_ms = monotonic_ms();
        if (strstr(line, UI_BOARD_PUSH_MARKER) != NULL) {
            g_app.ui_board_push_ms = seen_ms;
        } else if (strstr(line, UI_BOARD_POP_MARKER) != NULL) {
            g_app.ui_board_pop_ms = seen_ms;
        } else if (strstr(line, UI_INPUTS_ENABLED_MARKER) != NULL) {
            g_app.ui_inputs_enabled_ms = seen_ms;
        }
    }
    if (!g_app.enabled || g_app.generation != generation) {
        LeaveCriticalSection(&g_app.lock);
        return;
    }
    if (!initial_scan && strstr(line, INVASION_TIMEOUT_MARKER) != NULL &&
        live_timeout_is_too_old(line, local_clock_ms())) {
        bool had_armed_ticket = g_app.engine.has_armed_ticket;

        event = rq_engine_process_line(&g_app.engine,
                                       line,
                                       true,
                                       monotonic_ms(),
                                       now_wall_ms);
        if (had_armed_ticket && !g_app.engine.has_armed_ticket &&
            g_app.engine.awaiting_requeue_search) {
            rq_engine_disarm_pending(&g_app.engine);
            ZeroMemory(&event, sizeof(event));
            event.type = RQ_EVENT_REQUEUE_EXPIRED;
            event.total_search_ms = g_app.engine.cumulative_search_ms;
            event.cumulative_before_ms = g_app.engine.cumulative_search_ms;
            event.requeue_count = g_app.engine.requeue_count;
        }
    } else {
        event = rq_engine_process_line(&g_app.engine,
                                       line,
                                       initial_scan,
                                       monotonic_ms(),
                                       now_wall_ms);
    }
    apply_engine_event_locked(&event);
    LeaveCriticalSection(&g_app.lock);
    if (event.type != RQ_EVENT_NONE) {
        describe_engine_event(&event);
        (void)PostMessageW(g_app.window, WM_STATE_CHANGED, 0, 0);
    }
}

typedef struct line_buffer {
    char data[LINE_CAP];
    size_t length;
} line_buffer;

static void consume_log_bytes(line_buffer *lines,
                              const char *bytes,
                              DWORD byte_count,
                              bool initial_scan,
                              unsigned generation)
{
    for (DWORD index = 0; index < byte_count; ++index) {
        char byte = bytes[index];
        if (byte == '\n') {
            lines->data[lines->length] = '\0';
            process_log_line(lines->data, initial_scan, generation);
            lines->length = 0;
        } else if (byte != '\r') {
            if (lines->length + 1 < sizeof(lines->data)) {
                lines->data[lines->length++] = byte;
            } else {
                lines->length = 0;
                post_ui_event(L"An oversized log line was skipped safely.");
            }
        }
    }
}

static bool worker_snapshot(unsigned *generation)
{
    bool enabled;
    EnterCriticalSection(&g_app.lock);
    enabled = g_app.enabled;
    *generation = g_app.generation;
    LeaveCriticalSection(&g_app.lock);
    return enabled;
}

static bool recover_log(HANDLE file,
                        line_buffer *lines,
                        unsigned generation)
{
    char bytes[64 * 1024];
    DWORD count;
    LARGE_INTEGER beginning;
    LARGE_INTEGER snapshot_size;
    uint64_t remaining;
    rq_event recovered;

    beginning.QuadPart = 0;
    (void)SetFilePointerEx(file, beginning, NULL, FILE_BEGIN);
    snapshot_size.QuadPart = 0;
    (void)GetFileSizeEx(file, &snapshot_size);
    remaining = snapshot_size.QuadPart > 0 ? (uint64_t)snapshot_size.QuadPart : 0;
    lines->length = 0;
    EnterCriticalSection(&g_app.lock);
    if (!g_app.enabled || g_app.generation != generation) {
        LeaveCriticalSection(&g_app.lock);
        return false;
    }
    rq_engine_init(&g_app.engine);
    g_app.display_total_ms = 0;
    g_app.display_requeue_count = 0;
    g_app.search_live = false;
    LeaveCriticalSection(&g_app.lock);
    while (remaining > 0) {
        DWORD requested = remaining > sizeof(bytes) ? sizeof(bytes) : (DWORD)remaining;
        if (WaitForSingleObject(g_app.stop_event, 0) == WAIT_OBJECT_0 ||
            !generation_is_enabled(generation)) {
            return false;
        }
        if (!ReadFile(file, bytes, requested, &count, NULL) || count == 0) {
            break;
        }
        consume_log_bytes(lines, bytes, count, true, generation);
        remaining -= count;
    }
    EnterCriticalSection(&g_app.lock);
    if (!g_app.enabled || g_app.generation != generation) {
        LeaveCriticalSection(&g_app.lock);
        return false;
    }
    recovered = rq_engine_recovery_event(&g_app.engine);
    apply_engine_event_locked(&recovered);
    if (recovered.type == RQ_EVENT_NONE) {
        g_app.ui_state = UI_STATE_ARMED;
        set_status_locked(L"Armed, start the first Imposter search yourself");
    }
    LeaveCriticalSection(&g_app.lock);
    if (recovered.type != RQ_EVENT_NONE) {
        describe_engine_event(&recovered);
    }
    (void)PostMessageW(g_app.window, WM_STATE_CHANGED, 0, 0);
    return true;
}

static void tick_requeue(unsigned generation)
{
    rq_event event;
    int step = STEP_NONE;
    int note = NOTE_NONE;
    uint64_t now_ms = monotonic_ms();

    ZeroMemory(&event, sizeof(event));
    EnterCriticalSection(&g_app.lock);
    if (!g_app.enabled || g_app.generation != generation) {
        reset_action_sequence_locked();
        LeaveCriticalSection(&g_app.lock);
        return;
    }
    if (g_app.action_phase == ACTION_IDLE) {
        event = rq_engine_tick(&g_app.engine, now_ms);
        apply_engine_event_locked(&event);
        if (event.type == RQ_EVENT_REQUEUE_DUE) {
            g_app.action_phase = ACTION_AWAIT_BOARD;
            g_app.action_tab_tries = 0;
            g_app.action_tab_ms = now_ms;
            step = STEP_TAB;
        }
    } else if (!g_app.engine.pending) {
        /* The ticket resolved on its own, so no input belongs to it any more. */
        reset_action_sequence_locked();
    } else {
        step = advance_action_sequence_locked(now_ms);
    }
    note = g_app.action_note;
    g_app.action_note = NOTE_NONE;
    LeaveCriticalSection(&g_app.lock);

    if (event.type != RQ_EVENT_NONE) {
        describe_engine_event(&event);
        (void)PostMessageW(g_app.window, WM_STATE_CHANGED, 0, 0);
    }
    post_action_note(note);
    if (step == STEP_NONE) {
        return;
    }

    ZeroMemory(&event, sizeof(event));
    if (step == STEP_TAB) {
        bool posted = post_key_to_game(VK_TAB, generation);

        EnterCriticalSection(&g_app.lock);
        if (!g_app.enabled || g_app.generation != generation) {
            reset_action_sequence_locked();
        } else if (posted) {
            g_app.action_phase = ACTION_AWAIT_BOARD;
            g_app.action_tab_ms = monotonic_ms();
            ++g_app.action_tab_tries;
        } else {
            reset_action_sequence_locked();
            event = rq_engine_mark_requeue_failed(&g_app.engine);
            apply_engine_event_locked(&event);
        }
        LeaveCriticalSection(&g_app.lock);
    } else if (step == STEP_CONFIRM) {
        bool posted = click_start_button(generation);

        EnterCriticalSection(&g_app.lock);
        reset_action_sequence_locked();
        if (g_app.enabled && g_app.generation == generation) {
            event = posted ? rq_engine_mark_requeue_posted(
                                 &g_app.engine,
                                 monotonic_ms(),
                                 RQ_DEFAULT_VERIFY_TIMEOUT_MS)
                           : rq_engine_mark_requeue_failed(&g_app.engine);
            apply_engine_event_locked(&event);
        }
        LeaveCriticalSection(&g_app.lock);
    } else {
        EnterCriticalSection(&g_app.lock);
        reset_action_sequence_locked();
        if (g_app.enabled && g_app.generation == generation) {
            event = rq_engine_mark_requeue_failed(&g_app.engine);
            apply_engine_event_locked(&event);
        }
        LeaveCriticalSection(&g_app.lock);
        post_tab_failure_note();
    }
    if (event.type != RQ_EVENT_NONE) {
        describe_engine_event(&event);
    }
    (void)PostMessageW(g_app.window, WM_STATE_CHANGED, 0, 0);
}

static DWORD WINAPI monitor_thread(void *unused)
{
    HANDLE log_file = INVALID_HANDLE_VALUE;
    file_identity identity;
    line_buffer lines;
    unsigned generation = 0;
    uint64_t last_identity_check = 0;
    char bytes[64 * 1024];

    (void)unused;
    ZeroMemory(&identity, sizeof(identity));
    ZeroMemory(&lines, sizeof(lines));

    while (WaitForSingleObject(g_app.stop_event, 0) != WAIT_OBJECT_0) {
        unsigned current_generation;
        if (!worker_snapshot(&current_generation)) {
            if (log_file != INVALID_HANDLE_VALUE) {
                CloseHandle(log_file);
                log_file = INVALID_HANDLE_VALUE;
            }
            (void)WaitForSingleObject(g_app.wake_event, 250);
            continue;
        }
        if (generation != current_generation && log_file != INVALID_HANDLE_VALUE) {
            CloseHandle(log_file);
            log_file = INVALID_HANDLE_VALUE;
        }
        generation = current_generation;

        if (log_file == INVALID_HANDLE_VALUE) {
            log_file = CreateFileW(g_app.log_path,
                                   GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                       FILE_SHARE_DELETE,
                                   NULL,
                                   OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN,
                                   NULL);
            if (log_file == INVALID_HANDLE_VALUE) {
                set_status(L"Waiting for OPP.log");
                (void)WaitForSingleObject(g_app.wake_event, 500);
                continue;
            }
            (void)get_file_identity(log_file, &identity);
            if (!recover_log(log_file, &lines, generation)) {
                CloseHandle(log_file);
                log_file = INVALID_HANDLE_VALUE;
                ZeroMemory(&identity, sizeof(identity));
                lines.length = 0;
                continue;
            }
            last_identity_check = monotonic_ms();
        }

        {
            DWORD count = 0;
            if (!ReadFile(log_file, bytes, sizeof(bytes), &count, NULL)) {
                CloseHandle(log_file);
                log_file = INVALID_HANDLE_VALUE;
                continue;
            }
            if (count > 0) {
                consume_log_bytes(&lines, bytes, count, false, generation);
            }
        }
        tick_requeue(generation);

        if (monotonic_ms() - last_identity_check >= 1000) {
            HANDLE current = CreateFileW(g_app.log_path,
                                         GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE |
                                             FILE_SHARE_DELETE,
                                         NULL,
                                         OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL,
                                         NULL);
            LARGE_INTEGER size;
            LARGE_INTEGER position;
            file_identity current_identity;
            bool reopen = false;

            position.QuadPart = 0;
            if (current != INVALID_HANDLE_VALUE) {
                if (!get_file_identity(current, &current_identity) ||
                    !same_identity(&identity, &current_identity)) {
                    reopen = true;
                } else {
                    identity = current_identity;
                }
                CloseHandle(current);
            }
            if (GetFileSizeEx(log_file, &size) &&
                SetFilePointerEx(log_file, position, &position, FILE_CURRENT) &&
                size.QuadPart < position.QuadPart) {
                reopen = true;
            }
            if (reopen) {
                CloseHandle(log_file);
                log_file = INVALID_HANDLE_VALUE;
                ZeroMemory(&identity, sizeof(identity));
                lines.length = 0;
                post_ui_event(L"Game log rotated; current state was recovered.");
            }
            last_identity_check = monotonic_ms();
        }
        (void)WaitForSingleObject(g_app.wake_event, 100);
    }
    if (log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
    }
    return 0;
}

static void format_elapsed(uint64_t milliseconds,
                           wchar_t *output,
                           size_t capacity)
{
    uint64_t total_seconds = milliseconds / 1000;
    uint64_t hours = total_seconds / 3600;
    uint64_t minutes = (total_seconds / 60) % 60;
    uint64_t seconds = total_seconds % 60;

    if (hours > 0) {
        (void)StringCchPrintfW(output,
                               capacity,
                               L"%02llu:%02llu:%02llu",
                               (unsigned long long)hours,
                               (unsigned long long)minutes,
                               (unsigned long long)seconds);
    } else {
        (void)StringCchPrintfW(output,
                               capacity,
                               L"%02llu:%02llu",
                               (unsigned long long)minutes,
                               (unsigned long long)seconds);
    }
}

static uint64_t current_elapsed_locked(void)
{
    if (g_app.engine.has_armed_ticket) {
        uint64_t total = g_app.engine.cumulative_search_ms;
        if (g_app.search_live) {
            uint64_t now = monotonic_ms();
            total = g_app.search_base_ms;
            if (now >= g_app.search_anchor_mono_ms) {
                total += now - g_app.search_anchor_mono_ms;
            }
        }
        return total;
    }
    if (g_app.engine.pending || g_app.engine.awaiting_requeue_search) {
        return g_app.engine.cumulative_search_ms;
    }
    return g_app.display_total_ms;
}

static void refresh_ui(void)
{
    bool enabled;
    uint32_t count;
    uint64_t elapsed_ms;
    uint32_t attempt;
    uint32_t attempts_max;
    bool pending;
    start_button start;

    EnterCriticalSection(&g_app.lock);
    enabled = g_app.enabled;
    count = g_app.display_requeue_count;
    elapsed_ms = current_elapsed_locked();
    attempt = g_app.engine.requeue_attempt;
    attempts_max = g_app.engine.max_requeue_attempts;
    pending = g_app.engine.pending;
    start = g_app.start;
    g_app.display_state = g_app.ui_state;
    (void)StringCchCopyW(g_app.disp_status, STATUS_CAP, g_app.status);
    if (g_app.hide_request) {
        g_app.hide_request = false;
        g_app.hide_countdown = HIDE_COUNTDOWN_SECONDS;
    }
    LeaveCriticalSection(&g_app.lock);
    if (g_app.hide_countdown > 0) {
        (void)StringCchPrintfW(g_app.disp_status,
                               STATUS_CAP,
                               L"Match found, hiding in %d second%ls",
                               g_app.hide_countdown,
                               g_app.hide_countdown == 1 ? L"" : L"s");
    }

    g_app.display_enabled = enabled;
    format_elapsed(elapsed_ms, g_app.disp_timer, ARRAYSIZE(g_app.disp_timer));
    (void)StringCchPrintfW(g_app.disp_count,
                           ARRAYSIZE(g_app.disp_count),
                           L"%lu",
                           (unsigned long)count);
    if (pending) {
        (void)StringCchPrintfW(g_app.disp_attempt,
                               ARRAYSIZE(g_app.disp_attempt),
                               L"%lu of %lu",
                               (unsigned long)(attempt + 1 > attempts_max
                                                   ? attempts_max
                                                   : attempt + 1),
                               (unsigned long)attempts_max);
    } else {
        (void)StringCchCopyW(g_app.disp_attempt,
                             ARRAYSIZE(g_app.disp_attempt),
                             L"idle");
    }
    if (start.source == START_SOURCE_NONE) {
        (void)StringCchCopyW(
            g_app.disp_start, ARRAYSIZE(g_app.disp_start), L"none yet");
    } else {
        (void)StringCchPrintfW(g_app.disp_start,
                               ARRAYSIZE(g_app.disp_start),
                               L"%d, %d",
                               start.x,
                               start.y);
    }
    SetWindowTextW(g_app.toggle_button,
                   enabled ? L"DISABLE AUTO-REQUEUE" : L"ENABLE AUTO-REQUEUE");
    if (g_app.window != NULL) {
        InvalidateRect(g_app.window, NULL, FALSE);
    }
}

static void refresh_game_presence(void)
{
    g_app.game_running = find_game_process() != 0;
    g_app.game_window_found = g_app.game_running && find_game_window() != NULL;
}

static void trim_event_log(void)
{
    LRESULT lines = SendMessageW(g_app.event_log, EM_GETLINECOUNT, 0, 0);
    LRESULT excess = lines - EVENT_LOG_MAX_LINES;
    LRESULT cut;

    if (excess <= 0) {
        return;
    }
    cut = SendMessageW(g_app.event_log, EM_LINEINDEX, (WPARAM)excess, 0);
    if (cut > 0) {
        SendMessageW(g_app.event_log, EM_SETSEL, 0, (LPARAM)cut);
        SendMessageW(g_app.event_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
}

static void append_event_log(const wchar_t *message)
{
    SYSTEMTIME time;
    wchar_t line[700];
    int length;

    GetLocalTime(&time);
    (void)StringCchPrintfW(line,
                           ARRAYSIZE(line),
                           L"%02u:%02u:%02u  %ls\r\n",
                           (unsigned)time.wHour,
                           (unsigned)time.wMinute,
                           (unsigned)time.wSecond,
                           message);
    trim_event_log();
    length = GetWindowTextLengthW(g_app.event_log);
    SendMessageW(g_app.event_log, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    SendMessageW(g_app.event_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_app.event_log, EM_SCROLLCARET, 0, 0);
}

static void set_enabled(bool enabled)
{
    wchar_t reason[STATUS_CAP];

    if (enabled && !validate_install(reason, ARRAYSIZE(reason))) {
        set_status(reason);
        post_ui_event(L"Preflight failed: %ls", reason);
        return;
    }
    EnterCriticalSection(&g_app.lock);
    g_app.enabled = enabled;
    ++g_app.generation;
    rq_engine_init(&g_app.engine);
    g_app.display_total_ms = 0;
    g_app.display_requeue_count = 0;
    g_app.search_live = false;
    g_app.hide_request = false;
    g_app.ui_state = enabled ? UI_STATE_ARMED : UI_STATE_OFF;
    set_status_locked(enabled ? L"Armed, start the first Imposter search yourself"
                              : L"Automation is off");
    LeaveCriticalSection(&g_app.lock);
    g_app.hide_countdown = 0;
    if (enabled) {
        apply_topmost(true);
    }
    SetEvent(g_app.wake_event);
    if (enabled) {
        post_ui_event(L"Auto-requeue enabled.");
        if (find_game_process() == 0) {
            post_ui_event(L"The game is not running yet; it will be picked "
                          L"up when it starts.");
        }
    } else {
        post_ui_event(L"Auto-requeue disabled; pending action canceled.");
    }
    refresh_ui();
}

static bool browse_for_game(void)
{
    BROWSEINFOW info;
    PIDLIST_ABSOLUTE item;
    wchar_t selected[PATH_CAP];

    ZeroMemory(&info, sizeof(info));
    info.hwndOwner = g_app.window;
    info.lpszTitle = L"Select the 'The Outlast Trials' game folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    item = SHBrowseForFolderW(&info);
    if (item == NULL) {
        return false;
    }
    selected[0] = L'\0';
    if (!SHGetPathFromIDListW(item, selected)) {
        CoTaskMemFree(item);
        return false;
    }
    CoTaskMemFree(item);
    if (!game_dir_is_valid(selected)) {
        set_status(L"That folder does not contain the Outlast Trials executable.");
        return false;
    }
    if (g_app.enabled) {
        set_enabled(false);
    }
    (void)StringCchCopyW(g_app.game_dir, PATH_CAP, selected);
    update_derived_paths();
    (void)WritePrivateProfileStringW(
        L"Paths", L"GameDir", g_app.game_dir, g_app.settings_path);
    set_status(L"Game folder saved");
    post_ui_event(L"Game folder updated.");
    refresh_ui();
    return true;
}

static void open_log_folder(void)
{
    wchar_t folder[PATH_CAP];
    (void)StringCchCopyW(folder, PATH_CAP, g_app.log_path);
    if (parent_path(folder) && directory_exists(folder)) {
        (void)ShellExecuteW(
            g_app.window, L"open", folder, NULL, NULL, SW_SHOWNORMAL);
    } else {
        set_status(L"The game log folder does not exist yet.");
    }
}

static UINT system_dpi(void)
{
    HDC screen = GetDC(NULL);
    UINT dpi = screen != NULL ? (UINT)GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != NULL) {
        ReleaseDC(NULL, screen);
    }
    return dpi != 0 ? dpi : 96;
}

static int scale_ui(int value)
{
    return MulDiv(value, (int)g_app.ui_dpi, 96);
}

static HFONT create_font(int points, int weight, const wchar_t *face)
{
    return CreateFontW(-MulDiv(points, (int)g_app.ui_dpi, 72),
                       0,
                       0,
                       0,
                       weight,
                       FALSE,
                       FALSE,
                       FALSE,
                       DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,
                       face);
}

static HFONT create_font_exact(int points, int weight, const wchar_t *face)
{
    HFONT font = create_font(points, weight, face);
    HDC screen;
    HGDIOBJ previous;
    wchar_t actual[LF_FACESIZE];
    bool matches = false;

    if (font == NULL) {
        return NULL;
    }
    screen = GetDC(NULL);
    if (screen != NULL) {
        previous = SelectObject(screen, font);
        if (GetTextFaceW(screen, LF_FACESIZE, actual) > 0 &&
            _wcsicmp(actual, face) == 0) {
            matches = true;
        }
        SelectObject(screen, previous);
        ReleaseDC(NULL, screen);
    }
    if (!matches) {
        DeleteObject(font);
        return NULL;
    }
    return font;
}

static HFONT create_font_preferring(int points,
                                    int weight,
                                    const wchar_t *preferred,
                                    const wchar_t *fallback)
{
    HFONT font = create_font_exact(points, weight, preferred);
    return font != NULL ? font : create_font(points, weight, fallback);
}

/*
 * The window is a small always-on-top widget.  Collapsed, it shows the state
 * and the timer in one strip; while the pointer rests on it, it grows into
 * the full panel.  Leaving the widget collapses it again unless it is pinned.
 * Every child window reports pointer entry and exit here, so the panel stays
 * open while the pointer moves between the strip and its controls.
 */
/*
 * The widget floats above other windows until a match is found; then it
 * minimizes and stops floating, and floats again once restored or re-armed.
 */
static void apply_topmost(bool topmost)
{
    if (g_app.window == NULL || IsIconic(g_app.window)) {
        return;
    }
    SetWindowPos(g_app.window,
                 topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/*
 * Corners are rounded with a window region rather than the DWM corner
 * preference: with a layered window, DWM kept showing the image of the
 * previous size after every resize, which looked like a second widget.
 */
static void apply_window_region(int width, int height)
{
    int radius = scale_ui(UI_CORNER_RADIUS);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    if (region != NULL && SetWindowRgn(g_app.window, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

static void set_window_alpha(BYTE alpha)
{
    (void)SetLayeredWindowAttributes(g_app.window, 0, alpha, LWA_ALPHA);
}

static void save_window_position(void)
{
    RECT frame;
    wchar_t value[32];

    if (g_app.window == NULL || !GetWindowRect(g_app.window, &frame)) {
        return;
    }
    (void)StringCchPrintfW(value, ARRAYSIZE(value), L"%ld", (long)frame.left);
    (void)WritePrivateProfileStringW(
        L"Window", L"X", value, g_app.settings_path);
    (void)StringCchPrintfW(value,
                           ARRAYSIZE(value),
                           L"%ld",
                           (long)(frame.top + g_app.expand_shift));
    (void)WritePrivateProfileStringW(
        L"Window", L"Y", value, g_app.settings_path);
}

static void set_expanded(bool expanded)
{
    RECT frame;
    RECT work;
    int height = scale_ui(expanded ? UI_H_EXPANDED : UI_H_COMPACT);
    int top;

    if (g_app.window == NULL || !GetWindowRect(g_app.window, &frame)) {
        return;
    }
    top = frame.top;
    if (expanded && !g_app.expanded) {
        HMONITOR monitor = MonitorFromWindow(g_app.window,
                                             MONITOR_DEFAULTTONEAREST);
        MONITORINFO info;
        int overflow;

        info.cbSize = sizeof(info);
        work = frame;
        if (GetMonitorInfoW(monitor, &info)) {
            work = info.rcWork;
        }
        overflow = (frame.top + height) - work.bottom;
        if (overflow > 0) {
            g_app.expand_shift = overflow;
            if (frame.top - overflow < work.top) {
                g_app.expand_shift = frame.top - work.top;
            }
            top = frame.top - g_app.expand_shift;
        } else {
            g_app.expand_shift = 0;
        }
    } else if (!expanded && g_app.expanded) {
        top = frame.top + g_app.expand_shift;
        g_app.expand_shift = 0;
    }
    g_app.expanded = expanded;
    SetWindowPos(g_app.window,
                 NULL,
                 frame.left,
                 top,
                 frame.right - frame.left,
                 height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    apply_window_region(frame.right - frame.left, height);
    ShowWindow(g_app.event_log, expanded ? SW_SHOWNA : SW_HIDE);
    set_window_alpha(expanded ? 255 : UI_ALPHA_IDLE);
    InvalidateRect(g_app.window, NULL, FALSE);
}

static void hover_enter(void)
{
    KillTimer(g_app.window, TIMER_COLLAPSE);
    if (!g_app.expanded) {
        set_expanded(true);
    }
}

static void hover_leave(void)
{
    if (!g_app.pinned) {
        SetTimer(g_app.window, TIMER_COLLAPSE, HOVER_COLLAPSE_MS, NULL);
    }
}

static void track_leave(HWND window, bool nonclient)
{
    TRACKMOUSEEVENT track;
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE | (nonclient ? TME_NONCLIENT : 0);
    track.hwndTrack = window;
    track.dwHoverTime = 0;
    (void)TrackMouseEvent(&track);
}

static LRESULT CALLBACK child_subclass(HWND child,
                                       UINT message,
                                       WPARAM wparam,
                                       LPARAM lparam,
                                       UINT_PTR subclass_id,
                                       DWORD_PTR reference)
{
    bool is_button = reference != 0;

    switch (message) {
    case WM_MOUSEMOVE:
        hover_enter();
        if (g_app.hot_button != child) {
            if (is_button) {
                g_app.hot_button = child;
                InvalidateRect(child, NULL, FALSE);
            }
            track_leave(child, false);
        }
        break;
    case WM_MOUSELEAVE:
        hover_leave();
        if (g_app.hot_button == child) {
            g_app.hot_button = NULL;
            InvalidateRect(child, NULL, FALSE);
        }
        break;
    case WM_SETCURSOR:
        if (is_button) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_NCDESTROY:
        (void)RemoveWindowSubclass(child, child_subclass, subclass_id);
        break;
    default:
        break;
    }
    return DefSubclassProc(child, message, wparam, lparam);
}

static HWND create_button(HWND parent,
                          const wchar_t *text,
                          int identifier,
                          int x,
                          int y,
                          int width,
                          int height)
{
    HWND button = CreateWindowExW(0,
                                  L"BUTTON",
                                  text,
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  scale_ui(x),
                                  scale_ui(y),
                                  scale_ui(width),
                                  scale_ui(height),
                                  parent,
                                  (HMENU)(INT_PTR)identifier,
                                  g_app.instance,
                                  NULL);
    (void)SetWindowSubclass(button, child_subclass, 1, 1);
    return button;
}

static void show_main_window(void)
{
    ShowWindow(g_app.window, SW_SHOWNA);
    SetWindowPos(g_app.window,
                 HWND_TOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    hover_enter();
}

static void fill_rect_color(HDC dc, const RECT *rectangle, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, rectangle, brush);
    DeleteObject(brush);
}

static void frame_rect_color(HDC dc, const RECT *rectangle, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, rectangle, brush);
    DeleteObject(brush);
}

static void draw_rule(HDC dc, int y)
{
    RECT rule;
    rule.left = scale_ui(UI_PAD);
    rule.top = scale_ui(y);
    rule.right = scale_ui(UI_W - UI_PAD);
    rule.bottom = rule.top + 1;
    fill_rect_color(dc, &rule, CLR_LINE);
}

static int font_height(HDC dc, HFONT font)
{
    TEXTMETRICW metrics;
    SelectObject(dc, font);
    return GetTextMetricsW(dc, &metrics) ? (int)metrics.tmHeight : 0;
}

static int text_span(HDC dc, HFONT font, const wchar_t *text, int spacing)
{
    SIZE size;
    int length = (int)wcslen(text);

    size.cx = 0;
    size.cy = 0;
    SelectObject(dc, font);
    (void)GetTextExtentPoint32W(dc, text, length, &size);
    return size.cx + (length > 1 ? spacing * (length - 1) : 0);
}

static void draw_span(HDC dc,
                      HFONT font,
                      COLORREF color,
                      int spacing,
                      int x,
                      int y,
                      const wchar_t *text)
{
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetTextCharacterExtra(dc, spacing);
    (void)TextOutW(dc, x, y, text, (int)wcslen(text));
    SetTextCharacterExtra(dc, 0);
}

static void draw_button(const DRAWITEMSTRUCT *draw)
{
    wchar_t text[128];
    RECT rectangle = draw->rcItem;
    bool primary = draw->CtlID == ID_TOGGLE;
    bool glyph = draw->CtlID == ID_PIN || draw->CtlID == ID_CLOSE;
    bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    bool hot = draw->hwndItem == g_app.hot_button;
    HFONT font = primary ? g_app.fonts[F_BTN_MAIN] : g_app.fonts[F_BTN];
    COLORREF fill;
    COLORREF edge;
    COLORREF ink;
    int width;
    int x;
    int y;

    if (glyph) {
        bool active = draw->CtlID == ID_PIN && g_app.pinned;
        fill = pressed ? CLR_PANEL_PRESS : hot ? CLR_PANEL_HOT : CLR_BG;
        edge = fill;
        ink = active ? CLR_ACCENT : hot ? CLR_TEXT : CLR_TEXT_FAINT;
        if (g_app.fonts[F_GLYPH] != NULL) {
            font = g_app.fonts[F_GLYPH];
        }
    } else if (primary && !g_app.display_enabled) {
        fill = pressed ? CLR_ACCENT_PRESS : hot ? CLR_ACCENT_HOT : CLR_ACCENT;
        edge = fill;
        ink = RGB(255, 246, 248);
    } else if (primary) {
        fill = pressed ? CLR_PANEL_PRESS : hot ? CLR_PANEL_HOT : CLR_PANEL;
        edge = CLR_ACCENT;
        ink = hot ? CLR_ACCENT_HOT : CLR_ACCENT;
    } else {
        fill = pressed ? CLR_PANEL_PRESS : hot ? CLR_PANEL_HOT : CLR_PANEL;
        edge = hot ? RGB(70, 72, 82) : CLR_LINE;
        ink = hot ? CLR_TEXT : RGB(200, 202, 210);
    }
    fill_rect_color(draw->hDC, &rectangle, fill);
    frame_rect_color(draw->hDC, &rectangle, edge);

    text[0] = L'\0';
    if (glyph) {
        if (g_app.fonts[F_GLYPH] != NULL) {
            text[0] = draw->CtlID == ID_CLOSE
                          ? (wchar_t)0xE8BB
                          : (wchar_t)(g_app.pinned ? 0xE77A : 0xE718);
        } else {
            text[0] = draw->CtlID == ID_CLOSE ? L'x' : L'p';
        }
        text[1] = L'\0';
    } else {
        GetWindowTextW(draw->hwndItem, text, ARRAYSIZE(text));
    }
    SetBkMode(draw->hDC, TRANSPARENT);
    width = text_span(draw->hDC, font, text, glyph ? 0 : 1);
    x = rectangle.left + (rectangle.right - rectangle.left - width) / 2;
    y = rectangle.top +
        (rectangle.bottom - rectangle.top - font_height(draw->hDC, font)) / 2;
    draw_span(draw->hDC, font, ink, glyph ? 0 : 1, x, y, text);
}

static const wchar_t *state_label(int state)
{
    switch (state) {
    case UI_STATE_ARMED:
        return L"ARMED";
    case UI_STATE_SEARCHING:
        return L"SEARCHING";
    case UI_STATE_REQUEUE:
        return L"REQUEUING";
    case UI_STATE_DONE:
        return L"MATCH FOUND";
    default:
        return L"STANDBY";
    }
}

static void draw_stat(HDC dc,
                      int x,
                      int y,
                      const wchar_t *label,
                      const wchar_t *value,
                      COLORREF value_color)
{
    draw_span(dc, g_app.fonts[F_LABEL], CLR_TEXT_FAINT, 2, x, y, label);
    draw_span(dc,
              g_app.fonts[F_VALUE],
              value_color,
              0,
              x,
              y + scale_ui(13),
              value);
}

static void paint_window(HDC dc, const RECT *client)
{
    RECT rect;
    wchar_t footer[160];
    wchar_t requeues[48];
    const wchar_t *game_value;
    COLORREF game_color;
    COLORREF state_color;
    int left = scale_ui(UI_PAD);
    int right = scale_ui(UI_W - UI_PAD);
    int glyph_area = scale_ui(UI_GLYPH_BUTTON * 2 + 6);
    int column = (UI_W - 2 * UI_PAD) / 3;
    int width;

    FillRect(dc, client, g_app.background_brush);
    SetBkMode(dc, TRANSPARENT);

    /* Collapsed strip: state, timer, requeue count, game presence. */
    state_color = g_app.display_state == UI_STATE_OFF ? CLR_TEXT_FAINT
                                                      : CLR_ACCENT;
    if (g_app.display_state == UI_STATE_REQUEUE && (g_app.pulse & 1) != 0) {
        state_color = CLR_ACCENT_HOT;
    }
    draw_span(dc,
              g_app.fonts[F_STATE],
              state_color,
              3,
              left,
              scale_ui(11),
              state_label(g_app.display_state));
    if (g_app.hide_countdown > 0) {
        (void)StringCchPrintfW(requeues,
                               ARRAYSIZE(requeues),
                               L"HIDING IN %d S",
                               g_app.hide_countdown);
    } else {
        (void)StringCchPrintfW(requeues,
                               ARRAYSIZE(requeues),
                               L"%ls REQUEUE%ls",
                               g_app.disp_count,
                               wcscmp(g_app.disp_count, L"1") == 0 ? L"" : L"S");
    }
    width = text_span(dc, g_app.fonts[F_LABEL], requeues, 2);
    draw_span(dc,
              g_app.fonts[F_LABEL],
              CLR_TEXT_FAINT,
              2,
              right - glyph_area - width,
              scale_ui(12),
              requeues);
    rect.left = left;
    rect.top = scale_ui(26);
    rect.right = right;
    rect.bottom = scale_ui(58);
    SelectObject(dc, g_app.fonts[F_TIMER]);
    SetTextColor(dc, g_app.display_enabled ? CLR_TEXT : RGB(110, 112, 122));
    (void)DrawTextW(dc,
                    g_app.disp_timer,
                    -1,
                    &rect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (!g_app.game_running) {
        game_value = L"game not running";
        game_color = CLR_TEXT_FAINT;
    } else if (!g_app.game_window_found) {
        game_value = L"game window missing";
        game_color = CLR_ACCENT;
    } else {
        game_value = L"game window found";
        game_color = CLR_TEXT_DIM;
    }
    width = text_span(dc, g_app.fonts[F_SMALL], game_value, 0);
    draw_span(dc,
              g_app.fonts[F_SMALL],
              game_color,
              0,
              right - width,
              scale_ui(38),
              game_value);
    rect.left = right - width - scale_ui(12);
    rect.top = scale_ui(42);
    rect.right = rect.left + scale_ui(6);
    rect.bottom = rect.top + scale_ui(6);
    fill_rect_color(dc,
                    &rect,
                    g_app.game_window_found ? CLR_ACCENT : CLR_LINE);

    rect = *client;
    frame_rect_color(dc, &rect, CLR_LINE);
    if (!g_app.expanded) {
        return;
    }

    draw_rule(dc, UI_H_COMPACT);
    rect.left = left;
    rect.top = scale_ui(72);
    rect.right = right;
    rect.bottom = scale_ui(104);
    SelectObject(dc, g_app.fonts[F_STATUS]);
    SetTextColor(dc, CLR_TEXT_DIM);
    (void)DrawTextW(dc,
                    g_app.disp_status,
                    -1,
                    &rect,
                    DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);

    draw_stat(dc, left, scale_ui(108), L"REQUEUES", g_app.disp_count, CLR_TEXT);
    draw_stat(dc,
              left + scale_ui(column),
              scale_ui(108),
              L"ATTEMPT",
              g_app.disp_attempt,
              CLR_TEXT);
    draw_stat(dc,
              left + scale_ui(column * 2),
              scale_ui(108),
              L"START",
              g_app.disp_start,
              CLR_TEXT);
    draw_rule(dc, 148);

    draw_span(dc,
              g_app.fonts[F_LABEL],
              CLR_TEXT_FAINT,
              2,
              left,
              scale_ui(240),
              L"GAME FOLDER");
    rect.left = left;
    rect.top = scale_ui(253);
    rect.right = right;
    rect.bottom = scale_ui(268);
    SelectObject(dc, g_app.fonts[F_PATH]);
    SetTextColor(dc,
                 g_app.game_dir[0] != L'\0' ? RGB(196, 198, 206)
                                            : CLR_TEXT_DIM);
    (void)DrawTextW(dc,
                    g_app.game_dir[0] != L'\0'
                        ? g_app.game_dir
                        : L"Not located, press GAME FOLDER",
                    -1,
                    &rect,
                    DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);

    draw_span(dc,
              g_app.fonts[F_LABEL],
              CLR_TEXT_FAINT,
              2,
              left,
              scale_ui(276),
              L"ACTIVITY");
    rect.left = left;
    rect.top = scale_ui(290);
    rect.right = right;
    rect.bottom = scale_ui(366);
    fill_rect_color(dc, &rect, CLR_PANEL);
    frame_rect_color(dc, &rect, CLR_LINE);

    (void)StringCchPrintfW(footer,
                           ARRAYSIZE(footer),
                           L"%ls  by %ls  build %ls",
                           APP_VERSION,
                           APP_AUTHOR,
                           SUPPORTED_BUILD_ID);
    draw_span(dc,
              g_app.fonts[F_SMALL],
              CLR_TEXT_FAINT,
              0,
              left,
              scale_ui(372),
              footer);
}

static LRESULT CALLBACK window_procedure(HWND window,
                                         UINT message,
                                         WPARAM wparam,
                                         LPARAM lparam)
{
    switch (message) {
    case WM_CREATE: {
        DWORD square = 1;
        COLORREF none = 0xFFFFFFFE;
        const DWORD corner_preference = 33;
        const DWORD border_color = 34;
        int half = (UI_W - 2 * UI_PAD - 8) / 2;

        (void)DwmSetWindowAttribute(
            window, corner_preference, &square, sizeof(square));
        (void)DwmSetWindowAttribute(
            window, border_color, &none, sizeof(none));
        g_app.pin_button = create_button(window,
                                         L"",
                                         ID_PIN,
                                         UI_W - UI_PAD - UI_GLYPH_BUTTON * 2 - 4,
                                         10,
                                         UI_GLYPH_BUTTON,
                                         UI_GLYPH_BUTTON);
        g_app.close_button = create_button(window,
                                           L"",
                                           ID_CLOSE,
                                           UI_W - UI_PAD - UI_GLYPH_BUTTON,
                                           10,
                                           UI_GLYPH_BUTTON,
                                           UI_GLYPH_BUTTON);
        g_app.toggle_button = create_button(window,
                                            L"ENABLE AUTO-REQUEUE",
                                            ID_TOGGLE,
                                            UI_PAD,
                                            158,
                                            UI_W - 2 * UI_PAD,
                                            36);
        g_app.locate_button = create_button(
            window, L"GAME FOLDER", ID_LOCATE, UI_PAD, 202, half, 30);
        g_app.log_button = create_button(window,
                                         L"LOG FOLDER",
                                         ID_OPEN_LOG,
                                         UI_W - UI_PAD - half,
                                         202,
                                         half,
                                         30);
        g_app.event_log = CreateWindowExW(0,
                                          L"EDIT",
                                          L"",
                                          WS_CHILD | WS_VISIBLE |
                                              ES_MULTILINE | ES_AUTOVSCROLL |
                                              ES_READONLY,
                                          scale_ui(UI_PAD + 6),
                                          scale_ui(296),
                                          scale_ui(UI_W - 2 * UI_PAD - 12),
                                          scale_ui(64),
                                          window,
                                          NULL,
                                          g_app.instance,
                                          NULL);
        (void)SetWindowSubclass(g_app.event_log, child_subclass, 2, 0);
        SendMessageW(
            g_app.event_log, WM_SETFONT, (WPARAM)g_app.fonts[F_LOG], TRUE);
        SendMessageW(g_app.event_log,
                     EM_SETMARGINS,
                     EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale_ui(3), scale_ui(3)));
        SetTimer(window, TIMER_UI, 1000, NULL);
        append_event_log(L"Ready. Start the first Imposter search yourself.");
        refresh_game_presence();
        refresh_ui();
        return 0;
    }

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(window, message, wparam, lparam);
        /* The whole strip drags the widget; children keep their own hits. */
        return hit == HTCLIENT ? HTCAPTION : hit;
    }

    case WM_NCLBUTTONDBLCLK:
        return 0;

    case WM_NCMOUSEMOVE:
        hover_enter();
        if (!g_app.hover_tracked) {
            g_app.hover_tracked = true;
            track_leave(window, true);
        }
        break;

    case WM_NCMOUSELEAVE:
        g_app.hover_tracked = false;
        hover_leave();
        break;

    case WM_EXITSIZEMOVE:
        save_window_position();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_TOGGLE: {
            bool enabled;
            EnterCriticalSection(&g_app.lock);
            enabled = g_app.enabled;
            LeaveCriticalSection(&g_app.lock);
            set_enabled(!enabled);
            return 0;
        }
        case ID_LOCATE:
            (void)browse_for_game();
            return 0;
        case ID_OPEN_LOG:
            open_log_folder();
            return 0;
        case ID_PIN:
            g_app.pinned = !g_app.pinned;
            (void)WritePrivateProfileStringW(L"Window",
                                             L"Pinned",
                                             g_app.pinned ? L"1" : L"0",
                                             g_app.settings_path);
            if (g_app.pinned) {
                hover_enter();
            }
            InvalidateRect(g_app.pin_button, NULL, FALSE);
            return 0;
        case ID_CLOSE:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;

    case WM_DRAWITEM:
        draw_button((const DRAWITEMSTRUCT *)lparam);
        return TRUE;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC device = (HDC)wparam;
        SetTextColor(device, RGB(176, 178, 188));
        SetBkColor(device, CLR_PANEL);
        return (LRESULT)g_app.panel_brush;
    }

    case WM_CTLCOLORBTN:
        return (LRESULT)g_app.background_brush;

    case WM_ERASEBKGND: {
        RECT rectangle;
        GetClientRect(window, &rectangle);
        FillRect((HDC)wparam, &rectangle, g_app.background_brush);
        return TRUE;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC target = BeginPaint(window, &paint);
        RECT client;
        HDC memory;
        HBITMAP surface;

        GetClientRect(window, &client);
        memory = CreateCompatibleDC(target);
        surface = CreateCompatibleBitmap(target, client.right, client.bottom);
        if (memory != NULL && surface != NULL) {
            HGDIOBJ previous = SelectObject(memory, surface);
            paint_window(memory, &client);
            (void)BitBlt(target,
                         0,
                         0,
                         client.right,
                         client.bottom,
                         memory,
                         0,
                         0,
                         SRCCOPY);
            SelectObject(memory, previous);
        }
        if (surface != NULL) {
            DeleteObject(surface);
        }
        if (memory != NULL) {
            DeleteDC(memory);
        }
        EndPaint(window, &paint);
        return 0;
    }

    case WM_TIMER:
        if (wparam == TIMER_UI) {
            ++g_app.pulse;
            if ((g_app.pulse & 1) == 0) {
                refresh_game_presence();
            }
            if (g_app.hide_countdown > 0 && --g_app.hide_countdown == 0) {
                apply_topmost(false);
                ShowWindow(window, SW_MINIMIZE);
                post_ui_event(L"Hidden after the match; the widget floats "
                              L"again when auto-requeue is enabled.");
            }
            refresh_ui();
        } else if (wparam == TIMER_COLLAPSE) {
            KillTimer(window, TIMER_COLLAPSE);
            if (!g_app.pinned) {
                set_expanded(false);
            }
        }
        return 0;

    case WM_STATE_CHANGED:
        refresh_ui();
        return 0;

    case WM_UI_EVENT: {
        wchar_t *event = (wchar_t *)lparam;
        if (event != NULL) {
            append_event_log(event);
            HeapFree(GetProcessHeap(), 0, event);
        }
        return 0;
    }

    case WM_SHOW_EXISTING:
        show_main_window();
        return 0;

    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            SetWindowPos(window,
                         HWND_NOTOPMOST,
                         0,
                         0,
                         0,
                         0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (IsWindowVisible(window)) {
            /* Restored from the taskbar on purpose, so it floats again. */
            apply_topmost(true);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        g_app.quitting = true;
        KillTimer(window, TIMER_UI);
        KillTimer(window, TIMER_COLLAPSE);
        SetEvent(g_app.stop_event);
        SetEvent(g_app.wake_event);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* Restore the saved widget position when it is still on a monitor. */
static bool load_window_position(POINT *origin)
{
    RECT frame;

    origin->x = (LONG)GetPrivateProfileIntW(
        L"Window", L"X", LONG_MIN, g_app.settings_path);
    origin->y = (LONG)GetPrivateProfileIntW(
        L"Window", L"Y", LONG_MIN, g_app.settings_path);
    if (origin->x == LONG_MIN || origin->y == LONG_MIN) {
        return false;
    }
    frame.left = origin->x;
    frame.top = origin->y;
    frame.right = origin->x + scale_ui(UI_W);
    frame.bottom = origin->y + scale_ui(UI_H_COMPACT);
    return MonitorFromRect(&frame, MONITOR_DEFAULTTONULL) != NULL;
}

static void default_window_position(POINT *origin)
{
    RECT work;

    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    origin->x = work.right - scale_ui(UI_W) - scale_ui(16);
    origin->y = work.bottom - scale_ui(UI_H_COMPACT) - scale_ui(16);
}

static void cli_print(const wchar_t *format, ...)
{
    wchar_t wide[2048];
    char utf8[8192];
    va_list arguments;
    int count;
    DWORD written;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);

    if (output == NULL || output == INVALID_HANDLE_VALUE) {
        return;
    }
    va_start(arguments, format);
    (void)StringCchVPrintfW(wide, ARRAYSIZE(wide), format, arguments);
    va_end(arguments);
    count = WideCharToMultiByte(
        CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), NULL, NULL);
    if (count > 1) {
        (void)WriteFile(output, utf8, (DWORD)(count - 1), &written, NULL);
    }
}

static int run_diagnostics(void)
{
    wchar_t reason[STATUS_CAP];
    wchar_t build[64] = L"unavailable";
    HWND game_window;
    bool valid;

    (void)parse_build_id(build, ARRAYSIZE(build));
    valid = validate_install(reason, ARRAYSIZE(reason));
    game_window = find_game_window();
    cli_print(L"Outlast Requeue %ls, created by %ls\r\n",
              APP_VERSION,
              APP_AUTHOR);
    cli_print(L"Game: %ls\r\n", g_app.game_dir);
    cli_print(L"Log: %ls\r\n", g_app.log_path);
    cli_print(L"Build ID: %ls (last verified: %ls; not enforced)\r\n",
              build,
              SUPPORTED_BUILD_ID);
    cli_print(L"Game process: %ls\r\n",
              find_game_process() != 0 ? L"running" : L"not running");
    cli_print(L"Exact game window: %ls\r\n",
              game_window != NULL ? L"found" : L"not found");
    if (game_window != NULL) {
        client_capture capture;
        if (capture_game_client(game_window, &capture)) {
            POINT center;
            bool found = find_start_button(
                capture.pixels, capture.width, capture.height, &center);
            cli_print(L"Client area: %d by %d\r\n",
                      capture.width,
                      capture.height);
            if (found) {
                cli_print(L"START button: %ld, %ld\r\n",
                          (long)center.x,
                          (long)center.y);
            } else {
                cli_print(L"START button: not visible (open the Trial Board "
                          L"on the TRIAL tab and run this again)\r\n");
            }
            release_capture(&capture);
        } else {
            cli_print(L"Client capture: failed\r\n");
        }
    }
    if (g_app.start.source != START_SOURCE_NONE) {
        cli_print(L"Remembered START: %d, %d in %d by %d\r\n",
                  g_app.start.x,
                  g_app.start.y,
                  g_app.start.client_width,
                  g_app.start.client_height);
    }
    cli_print(L"Preflight: %ls%ls%ls\r\n",
              valid ? L"PASS" : L"FAIL",
              valid ? L"" : L": ",
              valid ? L"" : reason);
    return valid ? 0 : 2;
}

static void fill_test_rect(BYTE *pixels,
                           int width,
                           int left,
                           int top,
                           int right,
                           int bottom,
                           BYTE red,
                           BYTE green,
                           BYTE blue)
{
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            BYTE *pixel = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = 255;
        }
    }
}

static int run_start_detection_self_test(void)
{
    const int width = 640;
    const int height = 360;
    BYTE *pixels = HeapAlloc(GetProcessHeap(),
                             HEAP_ZERO_MEMORY,
                             (SIZE_T)width * (SIZE_T)height * 4);
    POINT center;
    int result = 0;

    if (pixels == NULL) {
        return 16;
    }
    /* Dark red board, a light polaroid frame higher up, small caption text. */
    fill_test_rect(pixels, width, 0, 0, width, height, 40, 8, 10);
    fill_test_rect(pixels, width, 14, 160, 100, 215, 230, 230, 230);
    fill_test_rect(pixels, width, 18, 164, 96, 211, 60, 50, 50);
    for (int glyph = 0; glyph < 24; ++glyph) {
        int x = 30 + glyph * 8;
        fill_test_rect(pixels, width, x, 276, x + 2, 288, 235, 235, 235);
        fill_test_rect(pixels, width, x, 276, x + 6, 278, 235, 235, 235);
    }
    if (find_start_button(pixels, width, height, &center)) {
        result = 17;
    }
    /* The START bar with dark lettering through its middle. */
    fill_test_rect(pixels, width, 35, 311, 194, 327, 219, 219, 219);
    fill_test_rect(pixels, width, 100, 316, 130, 322, 30, 30, 30);
    if (result == 0 &&
        (!find_start_button(pixels, width, height, &center) ||
         center.x < 110 || center.x > 118 || center.y < 317 ||
         center.y > 320)) {
        result = 18;
    }
    HeapFree(GetProcessHeap(), 0, pixels);
    return result;
}

static int run_self_test(void)
{
    rq_engine engine;
    rq_event event;
    int detection;

    rq_engine_init(&engine);
    event = rq_engine_process_line(
        &engine,
        "{\"type\":\"searching\",\"context\":\"invasion\","
        "\"ticketId\":\"one\",\"timestamp\":1000}",
        false,
        0,
        0);
    if (event.type != RQ_EVENT_SEARCHING) {
        return 10;
    }
    event = rq_engine_process_line(
        &engine,
        "{\"type\":\"timed_out\",\"context\":\"invasion\","
        "\"ticketId\":\"one\",\"timestamp\":5000}",
        false,
        100,
        0);
    if (event.type != RQ_EVENT_TIMEOUT || event.total_search_ms != 4000) {
        return 11;
    }
    if (rq_engine_tick(&engine, 2599).type != RQ_EVENT_NONE ||
        rq_engine_tick(&engine, 2600).type != RQ_EVENT_REQUEUE_DUE) {
        return 12;
    }
    event = rq_engine_mark_requeue_posted(&engine, 3000, 8000);
    if (event.type != RQ_EVENT_REQUEUE_POSTED ||
        rq_engine_tick(&engine, 10999).type != RQ_EVENT_NONE ||
        rq_engine_tick(&engine, 11000).type != RQ_EVENT_REQUEUE_RETRY) {
        return 13;
    }
    engine.requeue_attempt = engine.max_requeue_attempts;
    engine.pending_phase = RQ_PENDING_VERIFYING;
    engine.pending_verify_until_mono_ms = 12000;
    if (rq_engine_tick(&engine, 12000).type != RQ_EVENT_REQUEUE_UNCONFIRMED) {
        return 15;
    }
    {
        const char *stale =
            "[2026.08.17-14.51.54:170][859]OnlineCoreLogs: RTA: "
            "{\"type\":\"timed_out\",\"context\":\"invasion\"}";
        uint64_t written = 0;

        if (!parse_log_local_ms(stale, &written) ||
            !live_timeout_is_too_old(stale,
                                     written + LIVE_TIMEOUT_MAX_AGE_MS + 1) ||
            live_timeout_is_too_old(stale, written + LIVE_TIMEOUT_MAX_AGE_MS)) {
            return 14;
        }
    }
    detection = run_start_detection_self_test();
    if (detection != 0) {
        return detection;
    }
    cli_print(L"SELF_TEST_PASS\r\n");
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance,
                    HINSTANCE previous,
                    PWSTR command_line,
                    int show_command)
{
    WNDCLASSEXW window_class;
    MSG message;
    HWND existing;
    int argument_count = 0;
    wchar_t **arguments;
    int result = 0;
    bool ui_smoke_test = false;

    (void)previous;
    (void)show_command;
    ZeroMemory(&g_app, sizeof(g_app));
    g_app.instance = instance;
    InitializeCriticalSection(&g_app.lock);
    rq_engine_init(&g_app.engine);
    (void)StringCchCopyW(g_app.status, STATUS_CAP, L"Automation is off");
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
        DeleteCriticalSection(&g_app.lock);
        return 1;
    }
    if (!initialize_paths()) {
        CoUninitialize();
        DeleteCriticalSection(&g_app.lock);
        return 1;
    }

    arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments != NULL && argument_count >= 2) {
        if (_wcsicmp(arguments[1], L"--self-test") == 0) {
            result = run_self_test();
            LocalFree(arguments);
            CoUninitialize();
            DeleteCriticalSection(&g_app.lock);
            return result;
        }
        if (_wcsicmp(arguments[1], L"--diagnostics") == 0) {
            result = run_diagnostics();
            LocalFree(arguments);
            CoUninitialize();
            DeleteCriticalSection(&g_app.lock);
            return result;
        }
        if (_wcsicmp(arguments[1], L"--ui-smoke-test") == 0) {
            ui_smoke_test = true;
        }
    }
    if (arguments != NULL) {
        LocalFree(arguments);
    }
    (void)command_line;

    g_app.instance_mutex = CreateMutexW(NULL, FALSE, APP_MUTEX);
    if (g_app.instance_mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        existing = FindWindowW(APP_CLASS, NULL);
        if (existing != NULL) {
            (void)PostMessageW(existing, WM_SHOW_EXISTING, 0, 0);
        }
        if (g_app.instance_mutex != NULL) {
            CloseHandle(g_app.instance_mutex);
        }
        CoUninitialize();
        DeleteCriticalSection(&g_app.lock);
        return 0;
    }

    InitCommonControls();
    g_app.stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_app.wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_app.background_brush = CreateSolidBrush(CLR_BG);
    g_app.panel_brush = CreateSolidBrush(CLR_PANEL);
    g_app.ui_dpi = system_dpi();
    g_app.fonts[F_TITLE] = create_font(10, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_STATE] = create_font(8, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_TIMER] = create_font_preferring(
        20, FW_SEMIBOLD, L"Segoe UI Variable Display", L"Segoe UI");
    g_app.fonts[F_STATUS] = create_font(9, FW_NORMAL, L"Segoe UI");
    g_app.fonts[F_LABEL] = create_font(7, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_VALUE] = create_font(11, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_PATH] = create_font(8, FW_NORMAL, L"Segoe UI");
    g_app.fonts[F_BTN_MAIN] = create_font(9, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_BTN] = create_font(8, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_LOG] = create_font(8, FW_NORMAL, L"Consolas");
    g_app.fonts[F_SMALL] = create_font(7, FW_NORMAL, L"Segoe UI");
    g_app.fonts[F_GLYPH] =
        create_font_exact(9, FW_NORMAL, L"Segoe Fluent Icons");
    if (g_app.fonts[F_GLYPH] == NULL) {
        g_app.fonts[F_GLYPH] =
            create_font_exact(9, FW_NORMAL, L"Segoe MDL2 Assets");
    }
    g_app.icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));

    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hIcon = g_app.icon;
    window_class.hIconSm = g_app.icon;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = g_app.background_brush;
    window_class.lpszClassName = APP_CLASS;
    if (RegisterClassExW(&window_class) == 0) {
        result = 1;
        goto cleanup;
    }

    {
        POINT origin;
        DWORD style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

        if (!load_window_position(&origin)) {
            default_window_position(&origin);
        }
        g_app.pinned = GetPrivateProfileIntW(
                           L"Window", L"Pinned", 0, g_app.settings_path) != 0;
        g_app.expanded = false;
        g_app.window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST |
                                           WS_EX_LAYERED,
                                       APP_CLASS,
                                       APP_NAME,
                                       style,
                                       origin.x,
                                       origin.y,
                                       scale_ui(UI_W),
                                       scale_ui(UI_H_COMPACT),
                                       NULL,
                                       NULL,
                                       instance,
                                       NULL);
    }
    if (g_app.window == NULL) {
        result = 1;
        goto cleanup;
    }
    apply_window_region(scale_ui(UI_W), scale_ui(UI_H_COMPACT));
    set_window_alpha(255);
    if (!ui_smoke_test) {
        set_expanded(true);
    }
    if (ui_smoke_test) {
        refresh_ui();
        DestroyWindow(g_app.window);
        cli_print(L"UI_SMOKE_TEST_PASS\r\n");
        goto cleanup;
    }
    g_app.worker = CreateThread(NULL, 0, monitor_thread, NULL, 0, NULL);
    if (g_app.worker == NULL) {
        result = 1;
        DestroyWindow(g_app.window);
        goto cleanup;
    }
    /* The widget never needs keyboard focus, so it never takes it. */
    ShowWindow(g_app.window, SW_SHOWNOACTIVATE);
    UpdateWindow(g_app.window);
    if (!g_app.pinned) {
        SetTimer(g_app.window, TIMER_COLLAPSE, 4000, NULL);
    }
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

cleanup:
    if (g_app.stop_event != NULL) {
        SetEvent(g_app.stop_event);
    }
    if (g_app.wake_event != NULL) {
        SetEvent(g_app.wake_event);
    }
    if (g_app.worker != NULL) {
        /* Shared events and the critical section outlive the worker. */
        (void)WaitForSingleObject(g_app.worker, INFINITE);
        CloseHandle(g_app.worker);
    }
    if (g_app.stop_event != NULL) {
        CloseHandle(g_app.stop_event);
    }
    if (g_app.wake_event != NULL) {
        CloseHandle(g_app.wake_event);
    }
    for (size_t index = 0; index < F_COUNT; ++index) {
        if (g_app.fonts[index] != NULL) {
            DeleteObject(g_app.fonts[index]);
        }
    }
    if (g_app.background_brush != NULL) DeleteObject(g_app.background_brush);
    if (g_app.panel_brush != NULL) DeleteObject(g_app.panel_brush);
    if (g_app.instance_mutex != NULL) CloseHandle(g_app.instance_mutex);
    CoUninitialize();
    DeleteCriticalSection(&g_app.lock);
    return result;
}
