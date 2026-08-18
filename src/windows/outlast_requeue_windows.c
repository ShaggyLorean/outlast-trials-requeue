#define COBJMACROS
#include <windows.h>
#include <bcrypt.h>
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
#define APP_VERSION L"1.0.3"
#define APP_AUTHOR L"whispersgone"
#define STEAM_APP_ID L"1304930"
#define SUPPORTED_BUILD_ID L"24382135"
#define GAME_EXE L"TOTClient-Win64-Shipping.exe"
#define PAK_NAME L"zzz-OutlastRequeue_P.pak"
#define EXPECTED_PAK_SHA256 \
    L"1998125961ea66886ae41d71fe15ec2d555d045b980bc487ac5a6ea2a92d0c54"

#define TIMER_UI 1
#define WM_STATE_CHANGED (WM_APP + 2)
#define WM_UI_EVENT (WM_APP + 3)
#define WM_SHOW_EXISTING (WM_APP + 4)

#define ID_TOGGLE 1001
#define ID_LOCATE 1002
#define ID_INSTALL 1003
#define ID_OPEN_LOG 1004

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
#define ACTION_MAX_TAB_TRIES 40u
#define ACTION_NOTE_EVERY_TRIES 5u

#define CLR_BG RGB(10, 11, 14)
#define CLR_CARD RGB(22, 24, 30)
#define CLR_CARD_EDGE RGB(41, 44, 55)
#define CLR_LOG_BG RGB(15, 16, 20)
#define CLR_TEXT RGB(240, 241, 245)
#define CLR_TEXT_DIM RGB(157, 163, 175)
#define CLR_TEXT_FAINT RGB(99, 105, 117)
#define CLR_ACCENT RGB(226, 48, 71)
#define CLR_ACCENT_HOVER RGB(243, 72, 94)
#define CLR_ACCENT_PRESS RGB(166, 30, 47)

#define UI_CLIENT_W 600
#define UI_CLIENT_H 726

enum ui_font {
    F_TITLE,
    F_SUB,
    F_PILL,
    F_TIMER,
    F_STATUS,
    F_SMALL,
    F_TINY,
    F_PATH,
    F_BTN_MAIN,
    F_BTN,
    F_CHIP,
    F_LOG,
    F_GLYPH,
    F_COUNT
};

typedef struct file_identity {
    DWORD volume;
    DWORD index_high;
    DWORD index_low;
    BYTE prefix[LOG_PREFIX_CAP];
    DWORD prefix_length;
    bool valid;
} file_identity;

typedef struct app_state {
    HINSTANCE instance;
    HWND window;
    HWND toggle_button;
    HWND locate_button;
    HWND install_button;
    HWND log_button;
    HWND event_log;
    HWND hot_button;
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
    bool display_enabled;
    unsigned pulse;
    wchar_t disp_timer[32];
    wchar_t disp_count[64];
    wchar_t disp_status[STATUS_CAP];
    wchar_t status[STATUS_CAP];
    wchar_t game_dir[PATH_CAP];
    wchar_t log_path[PATH_CAP];
    wchar_t pak_path[PATH_CAP];
    wchar_t manifest_path[PATH_CAP];
    wchar_t settings_path[PATH_CAP];
    wchar_t module_dir[PATH_CAP];
} app_state;

static app_state g_app;

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

    (void)join_path(g_app.pak_path,
                    PATH_CAP,
                    g_app.game_dir,
                    L"OPP\\Content\\Paks\\" PAK_NAME);
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
    g_app.pak_path[0] = L'\0';
    g_app.manifest_path[0] = L'\0';
    return false;
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

static bool sha256_file(const wchar_t *path, wchar_t output[65])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    PUCHAR object = NULL;
    DWORD object_length = 0;
    DWORD hash_length = 0;
    DWORD result_length = 0;
    BYTE digest[32];
    BYTE buffer[64 * 1024];
    DWORD count;
    NTSTATUS status;
    bool success = false;
    static const wchar_t digits[] = L"0123456789abcdef";

    status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (status < 0) {
        goto cleanup;
    }
    status = BCryptGetProperty(algorithm,
                               BCRYPT_OBJECT_LENGTH,
                               (PUCHAR)&object_length,
                               sizeof(object_length),
                               &result_length,
                               0);
    if (status < 0) {
        goto cleanup;
    }
    status = BCryptGetProperty(algorithm,
                               BCRYPT_HASH_LENGTH,
                               (PUCHAR)&hash_length,
                               sizeof(hash_length),
                               &result_length,
                               0);
    if (status < 0 || hash_length != sizeof(digest)) {
        goto cleanup;
    }
    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, object_length);
    if (object == NULL) {
        goto cleanup;
    }
    status = BCryptCreateHash(
        algorithm, &hash, object, object_length, NULL, 0, 0);
    if (status < 0) {
        goto cleanup;
    }
    file = CreateFileW(path,
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN,
                       NULL);
    if (file == INVALID_HANDLE_VALUE) {
        goto cleanup;
    }
    do {
        if (!ReadFile(file, buffer, sizeof(buffer), &count, NULL)) {
            goto cleanup;
        }
        if (count > 0 && BCryptHashData(hash, buffer, count, 0) < 0) {
            goto cleanup;
        }
    } while (count > 0);
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) {
        goto cleanup;
    }
    for (size_t index = 0; index < sizeof(digest); ++index) {
        output[index * 2] = digits[digest[index] >> 4];
        output[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    output[64] = L'\0';
    success = true;

cleanup:
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    if (hash != NULL) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != NULL) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    if (object != NULL) {
        HeapFree(GetProcessHeap(), 0, object);
    }
    return success;
}

static bool validate_install(wchar_t *reason, size_t capacity)
{
    wchar_t digest[65];

    if (!game_dir_is_valid(g_app.game_dir)) {
        (void)StringCchCopyW(
            reason, capacity, L"The Outlast Trials folder was not found.");
        return false;
    }
    if (!file_exists(g_app.pak_path)) {
        (void)StringCchCopyW(
            reason, capacity, L"The required requeue PAK is not installed.");
        return false;
    }
    if (!sha256_file(g_app.pak_path, digest) ||
        _wcsicmp(digest, EXPECTED_PAK_SHA256) != 0) {
        (void)StringCchCopyW(
            reason, capacity, L"The installed requeue PAK failed verification.");
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
     * OFF commits under this lock, no later Tab/F can be posted by an older
     * action generation.  Holding it for the 80 ms pair also guarantees key-up.
     */
    EnterCriticalSection(&g_app.lock);
    if (!g_app.enabled || g_app.generation != generation) {
        LeaveCriticalSection(&g_app.lock);
        return false;
    }
    if (PostMessageW(game_window, WM_KEYDOWN, virtual_key, down)) {
        (void)WaitForSingleObject(g_app.stop_event, 80);
        result = PostMessageW(game_window, WM_KEYUP, virtual_key, up) != FALSE;
    }
    LeaveCriticalSection(&g_app.lock);
    return result;
}

static bool generation_is_enabled(unsigned generation)
{
    bool enabled;
    EnterCriticalSection(&g_app.lock);
    enabled = g_app.enabled && g_app.generation == generation;
    LeaveCriticalSection(&g_app.lock);
    return enabled;
}

/*
 * The Trial Board refuses input for the whole push transition, measured at
 * 1.26 to 1.32 seconds on a live client.  The previous fixed 1.2 second wait
 * put F within a few milliseconds of that boundary, so the requeue landed on
 * a disabled panel most of the time.  The sequence below is driven by the
 * monitor loop instead: it posts Tab, waits for the game to log that the board
 * is accepting input again, and only then posts F.  It never blocks the loop,
 * because the loop is also what feeds it those records.
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
         * before F means anything at all.
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
        post_ui_event(L"Trial Board was already open; reopening it for F.");
        break;
    case NOTE_TAB_RETRY:
        post_ui_event(L"Tab still not acknowledged; the game refuses it for "
                      L"now, still trying.");
        break;
    case NOTE_GATE_ASSUMED:
        post_ui_event(L"Board opened but never reported ready; sending F anyway.");
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
    case RQ_EVENT_SEARCHING:
        if (!event->requeue_confirmed && !event->recovered) {
            g_app.display_total_ms = 0;
        }
        set_status_locked(L"Searching for Imposter");
        break;
    case RQ_EVENT_TIMEOUT:
        g_app.display_total_ms = event->total_search_ms;
        set_status_locked(L"Timeout detected, preparing focusless requeue");
        break;
    case RQ_EVENT_SUCCEEDED:
        g_app.display_total_ms = event->total_search_ms;
        g_app.enabled = false;
        ++g_app.generation;
        set_status_locked(L"Match found, automation stopped");
        break;
    case RQ_EVENT_CANCELED:
        g_app.display_total_ms = event->total_search_ms;
        set_status_locked(L"Search canceled, state disarmed");
        break;
    case RQ_EVENT_DISCONNECTED:
        g_app.display_total_ms = event->total_search_ms;
        set_status_locked(L"Server disconnected, state safely disarmed");
        break;
    case RQ_EVENT_REQUEUE_POSTED:
        set_status_locked(L"Requeue posted, waiting for server confirmation");
        break;
    case RQ_EVENT_REQUEUE_RETRY:
        set_status_locked(L"No ticket yet, preparing another targeted attempt");
        break;
    case RQ_EVENT_REQUEUE_UNCONFIRMED:
        set_status_locked(L"Requeue was not confirmed, attempts exhausted");
        break;
    case RQ_EVENT_REQUEUE_EXPIRED:
        set_status_locked(L"Requeue timing window expired, no input sent");
        break;
    case RQ_EVENT_HELPER_FAILED:
        set_status_locked(L"Target game window unavailable, safely disarmed");
        break;
    case RQ_EVENT_REQUEUE_DUE:
        set_status_locked(L"Opening the Trial Board for a targeted requeue");
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
                          ? L"Recovered active Invasion search."
                          : (event->requeue_confirmed
                                 ? L"Requeue confirmed by a new Invasion ticket."
                                 : L"Invasion search detected."));
        break;
    case RQ_EVENT_TIMEOUT:
        post_ui_event(L"Matching Invasion ticket timed out.");
        break;
    case RQ_EVENT_SUCCEEDED:
        post_ui_event(L"Invasion match found; automation stopped.");
        break;
    case RQ_EVENT_CANCELED:
        post_ui_event(L"Invasion search canceled; state disarmed.");
        break;
    case RQ_EVENT_DISCONNECTED:
        post_ui_event(L"Game-server connection lost; no input sent.");
        break;
    case RQ_EVENT_REQUEUE_POSTED:
        post_ui_event(L"Targeted Tab → F posted without activating the game.");
        break;
    case RQ_EVENT_REQUEUE_RETRY:
        post_ui_event(L"No replacement ticket in time; retrying the sequence.");
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
        set_status_locked(L"Monitoring, start an Imposter search manually");
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
        /* The ticket resolved on its own, so no key belongs to it any more. */
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
        bool posted = post_key_to_game((UINT)L'F', generation);

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
    uint64_t total;
    if (g_app.engine.has_armed_ticket) {
        total = g_app.engine.cumulative_search_ms;
        if (g_app.engine.has_search_started) {
            uint64_t now = wall_clock_ms();
            if (now >= g_app.engine.search_started_ms) {
                total += now - g_app.engine.search_started_ms;
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

    EnterCriticalSection(&g_app.lock);
    enabled = g_app.enabled;
    count = g_app.display_requeue_count;
    elapsed_ms = current_elapsed_locked();
    (void)StringCchCopyW(g_app.disp_status, STATUS_CAP, g_app.status);
    LeaveCriticalSection(&g_app.lock);

    g_app.display_enabled = enabled;
    format_elapsed(elapsed_ms, g_app.disp_timer, ARRAYSIZE(g_app.disp_timer));
    (void)StringCchPrintfW(g_app.disp_count,
                           ARRAYSIZE(g_app.disp_count),
                           L"VERIFIED REQUEUES  ·  %lu",
                           (unsigned long)count);
    SetWindowTextW(g_app.toggle_button,
                   enabled ? L"DISABLE AUTO-REQUEUE" : L"ENABLE AUTO-REQUEUE");
    if (g_app.window != NULL) {
        InvalidateRect(g_app.window, NULL, FALSE);
    }
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
                           L"[%02u:%02u:%02u] %ls\r\n",
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
    set_status_locked(enabled ? L"Monitoring, start an Imposter search manually"
                              : L"Automation is off");
    LeaveCriticalSection(&g_app.lock);
    SetEvent(g_app.wake_event);
    post_ui_event(enabled ? L"Auto-requeue enabled."
                          : L"Auto-requeue disabled; pending action canceled.");
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
    set_status(L"Game folder saved, verify or install the PAK");
    post_ui_event(L"Game folder updated.");
    refresh_ui();
    return true;
}

static bool locate_payload(wchar_t *output, size_t capacity)
{
    wchar_t payload_dir[PATH_CAP];
    if (join_path(payload_dir, PATH_CAP, g_app.module_dir, L"payload") &&
        join_path(output, capacity, payload_dir, PAK_NAME) &&
        file_exists(output)) {
        return true;
    }
    return join_path(output, capacity, g_app.module_dir, PAK_NAME) &&
           file_exists(output);
}

static void install_or_repair_pak(void)
{
    wchar_t source[PATH_CAP];
    wchar_t temporary[PATH_CAP];
    wchar_t digest[65];
    wchar_t existing_digest[65];
    bool destination_exists;

    if (!game_dir_is_valid(g_app.game_dir)) {
        set_status(L"Locate the game folder before installing the PAK.");
        return;
    }
    if (find_game_process() != 0) {
        set_status(L"Close The Outlast Trials before changing the PAK.");
        post_ui_event(L"PAK install blocked while the game is running.");
        return;
    }
    if (!locate_payload(source, PATH_CAP) || !sha256_file(source, digest) ||
        _wcsicmp(digest, EXPECTED_PAK_SHA256) != 0) {
        set_status(L"The release payload PAK is missing or invalid.");
        return;
    }
    destination_exists = file_exists(g_app.pak_path);
    if (destination_exists && sha256_file(g_app.pak_path, existing_digest) &&
        _wcsicmp(existing_digest, EXPECTED_PAK_SHA256) == 0) {
        set_status(L"The verified requeue PAK is already installed.");
        return;
    }
    if (destination_exists) {
        set_status(L"A different PAK uses this name; it was not overwritten.");
        post_ui_event(L"Back up or remove the conflicting PAK manually first.");
        return;
    }
    (void)StringCchPrintfW(temporary,
                           PATH_CAP,
                           L"%ls.installing-%lu",
                           g_app.pak_path,
                           (unsigned long)GetCurrentProcessId());
    (void)DeleteFileW(temporary);
    if (!CopyFileW(source, temporary, FALSE) ||
        !sha256_file(temporary, existing_digest) ||
        _wcsicmp(existing_digest, EXPECTED_PAK_SHA256) != 0 ||
        !MoveFileExW(temporary, g_app.pak_path, MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        (void)DeleteFileW(temporary);
        if (error == ERROR_ACCESS_DENIED) {
            set_status(L"Access denied. Run Install.bat from the release folder.");
        } else {
            set_status(L"PAK installation failed; the existing file was preserved.");
        }
        return;
    }
    set_status(L"PAK installed and SHA-256 verified.");
    post_ui_event(L"PAK installed atomically and verified.");
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

static void apply_dark_titlebar(HWND window)
{
    BOOL dark = TRUE;
    COLORREF chrome = CLR_BG;
    DWORD rounded = 2;
    const DWORD immersive_dark_mode = 20;
    const DWORD immersive_dark_mode_legacy = 19;
    const DWORD caption_color = 35;
    const DWORD border_color = 34;
    const DWORD corner_preference = 33;

    if (FAILED(DwmSetWindowAttribute(
            window, immersive_dark_mode, &dark, sizeof(dark)))) {
        (void)DwmSetWindowAttribute(
            window, immersive_dark_mode_legacy, &dark, sizeof(dark));
    }
    (void)DwmSetWindowAttribute(
        window, caption_color, &chrome, sizeof(chrome));
    (void)DwmSetWindowAttribute(
        window, border_color, &chrome, sizeof(chrome));
    (void)DwmSetWindowAttribute(
        window, corner_preference, &rounded, sizeof(rounded));
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

static LRESULT CALLBACK button_subclass(HWND button,
                                        UINT message,
                                        WPARAM wparam,
                                        LPARAM lparam,
                                        UINT_PTR subclass_id,
                                        DWORD_PTR reference)
{
    (void)reference;
    switch (message) {
    case WM_MOUSEMOVE:
        if (g_app.hot_button != button) {
            TRACKMOUSEEVENT track;
            g_app.hot_button = button;
            InvalidateRect(button, NULL, FALSE);
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = button;
            track.dwHoverTime = 0;
            (void)TrackMouseEvent(&track);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_app.hot_button == button) {
            g_app.hot_button = NULL;
            InvalidateRect(button, NULL, FALSE);
        }
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    case WM_NCDESTROY:
        (void)RemoveWindowSubclass(button, button_subclass, subclass_id);
        break;
    default:
        break;
    }
    return DefSubclassProc(button, message, wparam, lparam);
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
    (void)SetWindowSubclass(button, button_subclass, 1, 0);
    return button;
}

static void show_main_window(void)
{
    ShowWindow(g_app.window, SW_RESTORE);
    SetWindowPos(g_app.window,
                 HWND_TOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void fill_rounded(HDC dc,
                         const RECT *rectangle,
                         int radius,
                         COLORREF fill,
                         COLORREF edge)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);

    RoundRect(dc,
              rectangle->left,
              rectangle->top,
              rectangle->right,
              rectangle->bottom,
              scale_ui(radius),
              scale_ui(radius));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void fill_circle(HDC dc, int x, int y, int diameter, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));

    Ellipse(dc, x, y, x + diameter + 1, y + diameter + 1);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(brush);
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

static wchar_t button_glyph(UINT identifier)
{
    switch (identifier) {
    case ID_LOCATE:
        return 0xE8B7; /* folder */
    case ID_INSTALL:
        return 0xE896; /* download */
    case ID_OPEN_LOG:
        return 0xE8A5; /* document */
    default:
        return 0;
    }
}

static void draw_button(const DRAWITEMSTRUCT *draw)
{
    wchar_t text[128];
    wchar_t glyph[2];
    RECT rectangle = draw->rcItem;
    bool primary = draw->CtlID == ID_TOGGLE;
    bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    bool hot = draw->hwndItem == g_app.hot_button;
    HFONT font = primary ? g_app.fonts[F_BTN_MAIN] : g_app.fonts[F_BTN];
    COLORREF fill;
    COLORREF edge;
    COLORREF ink;
    int width;
    int x;
    int y;

    FillRect(draw->hDC, &rectangle, g_app.background_brush);
    glyph[0] = primary ? L'\0' : button_glyph(draw->CtlID);
    glyph[1] = L'\0';
    if (primary && !g_app.display_enabled) {
        fill = pressed ? CLR_ACCENT_PRESS : hot ? CLR_ACCENT_HOVER : CLR_ACCENT;
        edge = fill;
        ink = RGB(255, 245, 247);
    } else if (primary) {
        fill = pressed ? RGB(28, 13, 17)
                       : hot ? RGB(54, 22, 30) : RGB(41, 18, 24);
        edge = RGB(112, 38, 51);
        ink = RGB(255, 128, 141);
    } else {
        fill = pressed ? RGB(18, 20, 25)
                       : hot ? RGB(37, 41, 50) : RGB(27, 30, 37);
        edge = hot ? RGB(66, 71, 85) : RGB(46, 50, 61);
        ink = hot ? CLR_TEXT : RGB(210, 214, 223);
    }
    fill_rounded(draw->hDC, &rectangle, primary ? 12 : 10, fill, edge);

    text[0] = L'\0';
    GetWindowTextW(draw->hwndItem, text, ARRAYSIZE(text));
    SetBkMode(draw->hDC, TRANSPARENT);
    width = text_span(draw->hDC, font, text, 1);
    if (glyph[0] != L'\0' && g_app.fonts[F_GLYPH] != NULL) {
        int glyph_width = text_span(draw->hDC, g_app.fonts[F_GLYPH], glyph, 0);
        int gap = scale_ui(9);
        int total = glyph_width + gap + width;

        x = rectangle.left + (rectangle.right - rectangle.left - total) / 2;
        y = rectangle.top +
            (rectangle.bottom - rectangle.top -
             font_height(draw->hDC, g_app.fonts[F_GLYPH])) / 2;
        draw_span(draw->hDC,
                  g_app.fonts[F_GLYPH],
                  hot ? CLR_TEXT : RGB(146, 152, 165),
                  0,
                  x,
                  y,
                  glyph);
        y = rectangle.top +
            (rectangle.bottom - rectangle.top -
             font_height(draw->hDC, font)) / 2;
        draw_span(draw->hDC, font, ink, 1, x + glyph_width + gap, y, text);
    } else {
        x = rectangle.left + (rectangle.right - rectangle.left - width) / 2;
        y = rectangle.top +
            (rectangle.bottom - rectangle.top -
             font_height(draw->hDC, font)) / 2;
        draw_span(draw->hDC, font, ink, 1, x, y, text);
    }
}

static void paint_window(HDC dc, const RECT *client)
{
    RECT rect;
    HBRUSH rule_brush;
    wchar_t footer[192];
    int width;

    FillRect(dc, client, g_app.background_brush);
    SetBkMode(dc, TRANSPARENT);

    if (g_app.icon != NULL) {
        (void)DrawIconEx(dc,
                         scale_ui(28),
                         scale_ui(24),
                         g_app.icon,
                         scale_ui(40),
                         scale_ui(40),
                         0,
                         NULL,
                         DI_NORMAL);
    }
    draw_span(dc,
              g_app.fonts[F_TITLE],
              CLR_TEXT,
              1,
              scale_ui(82),
              scale_ui(20),
              L"OUTLAST REQUEUE");
    draw_span(dc,
              g_app.fonts[F_SUB],
              CLR_TEXT_DIM,
              0,
              scale_ui(83),
              scale_ui(56),
              L"Focusless Invasion matchmaking recovery");
    {
        const wchar_t *chip = L"v" APP_VERSION;
        int chip_text = text_span(dc, g_app.fonts[F_CHIP], chip, 1);
        int chip_height = scale_ui(22);

        rect.right = scale_ui(UI_CLIENT_W - 28);
        rect.left = rect.right - chip_text - scale_ui(24);
        rect.top = scale_ui(30);
        rect.bottom = rect.top + chip_height;
        fill_rounded(dc, &rect, 11, CLR_CARD, CLR_CARD_EDGE);
        draw_span(dc,
                  g_app.fonts[F_CHIP],
                  CLR_TEXT_DIM,
                  1,
                  rect.left + scale_ui(12),
                  rect.top +
                      (chip_height - font_height(dc, g_app.fonts[F_CHIP])) / 2,
                  chip);
    }

    rect.left = scale_ui(28);
    rect.top = scale_ui(96);
    rect.right = scale_ui(572);
    rect.bottom = scale_ui(336);
    fill_rounded(dc, &rect, 14, CLR_CARD, CLR_CARD_EDGE);
    {
        const wchar_t *pill = g_app.display_enabled ? L"AUTO-REQUEUE ARMED"
                                                    : L"STANDBY";
        COLORREF pill_fill;
        COLORREF pill_edge;
        COLORREF pill_ink;
        COLORREF dot;
        int pill_text = text_span(dc, g_app.fonts[F_PILL], pill, 2);
        int pill_height = scale_ui(30);
        int dot_diameter = scale_ui(8);
        int pad = scale_ui(14);
        int gap = scale_ui(8);
        int pill_width = pad + dot_diameter + gap + pill_text + pad;
        RECT pill_rect;

        if (g_app.display_enabled) {
            pill_fill = RGB(45, 18, 24);
            pill_edge = RGB(99, 32, 45);
            pill_ink = RGB(255, 122, 136);
            dot = (g_app.pulse & 1) != 0 ? RGB(255, 92, 108)
                                         : RGB(196, 44, 62);
        } else {
            pill_fill = RGB(28, 30, 38);
            pill_edge = RGB(50, 54, 66);
            pill_ink = RGB(160, 166, 178);
            dot = RGB(112, 118, 130);
        }
        pill_rect.left = scale_ui(300) - pill_width / 2;
        pill_rect.top = scale_ui(120);
        pill_rect.right = pill_rect.left + pill_width;
        pill_rect.bottom = pill_rect.top + pill_height;
        fill_rounded(dc, &pill_rect, 15, pill_fill, pill_edge);
        fill_circle(dc,
                    pill_rect.left + pad,
                    pill_rect.top + (pill_height - dot_diameter) / 2,
                    dot_diameter,
                    dot);
        draw_span(dc,
                  g_app.fonts[F_PILL],
                  pill_ink,
                  2,
                  pill_rect.left + pad + dot_diameter + gap,
                  pill_rect.top +
                      (pill_height - font_height(dc, g_app.fonts[F_PILL])) / 2,
                  pill);
    }
    rect.left = scale_ui(28);
    rect.top = scale_ui(156);
    rect.right = scale_ui(572);
    rect.bottom = scale_ui(244);
    SelectObject(dc, g_app.fonts[F_TIMER]);
    SetTextColor(dc, g_app.display_enabled ? CLR_TEXT : RGB(122, 128, 140));
    (void)DrawTextW(dc,
                    g_app.disp_timer,
                    -1,
                    &rect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    rect.left = scale_ui(58);
    rect.top = scale_ui(248);
    rect.right = scale_ui(542);
    rect.bottom = scale_ui(286);
    SelectObject(dc, g_app.fonts[F_STATUS]);
    SetTextColor(dc, CLR_TEXT_DIM);
    (void)DrawTextW(dc,
                    g_app.disp_status,
                    -1,
                    &rect,
                    DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    width = text_span(dc, g_app.fonts[F_TINY], g_app.disp_count, 2);
    draw_span(dc,
              g_app.fonts[F_TINY],
              CLR_TEXT_FAINT,
              2,
              scale_ui(300) - width / 2,
              scale_ui(296),
              g_app.disp_count);

    draw_span(dc,
              g_app.fonts[F_SMALL],
              CLR_TEXT_FAINT,
              2,
              scale_ui(30),
              scale_ui(480),
              L"GAME FOLDER");
    rect.left = scale_ui(30);
    rect.top = scale_ui(496);
    rect.right = scale_ui(570);
    rect.bottom = scale_ui(514);
    SelectObject(dc, g_app.fonts[F_PATH]);
    SetTextColor(dc,
                 g_app.game_dir[0] != L'\0' ? RGB(196, 201, 211)
                                            : CLR_TEXT_FAINT);
    (void)DrawTextW(dc,
                    g_app.game_dir[0] != L'\0'
                        ? g_app.game_dir
                        : L"Not located, press LOCATE GAME",
                    -1,
                    &rect,
                    DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);

    draw_span(dc,
              g_app.fonts[F_SMALL],
              CLR_TEXT_FAINT,
              2,
              scale_ui(30),
              scale_ui(526),
              L"ACTIVITY");
    width = text_span(dc, g_app.fonts[F_SMALL], L"ACTIVITY", 2);
    rect.left = scale_ui(30) + width + scale_ui(12);
    rect.top = scale_ui(526) + font_height(dc, g_app.fonts[F_SMALL]) / 2;
    rect.right = scale_ui(572);
    rect.bottom = rect.top + 1;
    rule_brush = CreateSolidBrush(CLR_CARD_EDGE);
    FillRect(dc, &rect, rule_brush);
    DeleteObject(rule_brush);

    rect.left = scale_ui(28);
    rect.top = scale_ui(546);
    rect.right = scale_ui(572);
    rect.bottom = scale_ui(694);
    fill_rounded(dc, &rect, 12, CLR_LOG_BG, CLR_CARD_EDGE);

    (void)StringCchPrintfW(footer,
                           ARRAYSIZE(footer),
                           L"v%ls   ·   by %ls   ·   verified on build %ls",
                           APP_VERSION,
                           APP_AUTHOR,
                           SUPPORTED_BUILD_ID);
    width = text_span(dc, g_app.fonts[F_TINY], footer, 1);
    draw_span(dc,
              g_app.fonts[F_TINY],
              CLR_TEXT_FAINT,
              1,
              scale_ui(300) - width / 2,
              scale_ui(704),
              footer);
}

static LRESULT CALLBACK window_procedure(HWND window,
                                         UINT message,
                                         WPARAM wparam,
                                         LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        apply_dark_titlebar(window);
        g_app.toggle_button = create_button(
            window, L"ENABLE AUTO-REQUEUE", ID_TOGGLE, 28, 352, 544, 52);
        g_app.locate_button = create_button(
            window, L"LOCATE GAME", ID_LOCATE, 28, 418, 173, 44);
        g_app.install_button = create_button(
            window, L"INSTALL PAK", ID_INSTALL, 213, 418, 173, 44);
        g_app.log_button = create_button(
            window, L"OPEN LOG", ID_OPEN_LOG, 398, 418, 174, 44);
        g_app.event_log = CreateWindowExW(0,
                                          L"EDIT",
                                          L"",
                                          WS_CHILD | WS_VISIBLE |
                                              ES_MULTILINE | ES_AUTOVSCROLL |
                                              ES_READONLY,
                                          scale_ui(40),
                                          scale_ui(556),
                                          scale_ui(520),
                                          scale_ui(128),
                                          window,
                                          NULL,
                                          g_app.instance,
                                          NULL);
        SendMessageW(
            g_app.event_log, WM_SETFONT, (WPARAM)g_app.fonts[F_LOG], TRUE);
        SendMessageW(g_app.event_log,
                     EM_SETMARGINS,
                     EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale_ui(6), scale_ui(6)));
        SetTimer(window, TIMER_UI, 1000, NULL);
        append_event_log(L"Ready. Start the first Imposter search manually.");
        refresh_ui();
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
        case ID_INSTALL:
            install_or_repair_pak();
            refresh_ui();
            return 0;
        case ID_OPEN_LOG:
            open_log_folder();
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
        SetTextColor(device, RGB(176, 182, 194));
        SetBkColor(device, CLR_LOG_BG);
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
            refresh_ui();
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
            SetWindowPos(window,
                         HWND_TOPMOST,
                         0,
                         0,
                         0,
                         0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        g_app.quitting = true;
        KillTimer(window, TIMER_UI);
        SetEvent(g_app.stop_event);
        SetEvent(g_app.wake_event);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
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
    wchar_t digest[65] = L"unavailable";
    bool valid;

    (void)parse_build_id(build, ARRAYSIZE(build));
    (void)sha256_file(g_app.pak_path, digest);
    valid = validate_install(reason, ARRAYSIZE(reason));
    cli_print(L"Outlast Requeue %ls, created by %ls\r\n",
              APP_VERSION,
              APP_AUTHOR);
    cli_print(L"Game: %ls\r\n", g_app.game_dir);
    cli_print(L"Log: %ls\r\n", g_app.log_path);
    cli_print(L"PAK: %ls\r\n", g_app.pak_path);
    cli_print(L"Build ID: %ls (last verified: %ls; not enforced)\r\n",
              build,
              SUPPORTED_BUILD_ID);
    cli_print(L"PAK SHA-256: %ls\r\n", digest);
    cli_print(L"Game process: %ls\r\n",
              find_game_process() != 0 ? L"running" : L"not running");
    cli_print(L"Exact game window: %ls\r\n",
              find_game_window() != NULL ? L"found" : L"not found");
    cli_print(L"Preflight: %ls%ls%ls\r\n",
              valid ? L"PASS" : L"FAIL",
              valid ? L"" : L": ",
              valid ? L"" : reason);
    return valid ? 0 : 2;
}

static int run_self_test(void)
{
    rq_engine engine;
    rq_event event;

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
    g_app.panel_brush = CreateSolidBrush(CLR_LOG_BG);
    g_app.ui_dpi = system_dpi();
    g_app.fonts[F_TITLE] = create_font_preferring(
        21, FW_BOLD, L"Segoe UI Variable Display", L"Segoe UI");
    g_app.fonts[F_SUB] = create_font(10, FW_NORMAL, L"Segoe UI");
    g_app.fonts[F_PILL] = create_font(9, FW_BOLD, L"Segoe UI");
    g_app.fonts[F_TIMER] = create_font_preferring(
        52, FW_SEMIBOLD, L"Segoe UI Variable Display", L"Segoe UI");
    g_app.fonts[F_STATUS] = create_font(10, FW_NORMAL, L"Segoe UI");
    g_app.fonts[F_SMALL] = create_font(8, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_TINY] = create_font(8, FW_NORMAL, L"Segoe UI");
    g_app.fonts[F_PATH] = create_font(9, FW_NORMAL, L"Segoe UI");
    g_app.fonts[F_BTN_MAIN] = create_font(11, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_BTN] = create_font(9, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_CHIP] = create_font(8, FW_SEMIBOLD, L"Segoe UI");
    g_app.fonts[F_LOG] = create_font(9, FW_NORMAL, L"Consolas");
    g_app.fonts[F_GLYPH] =
        create_font_exact(12, FW_NORMAL, L"Segoe Fluent Icons");
    if (g_app.fonts[F_GLYPH] == NULL) {
        g_app.fonts[F_GLYPH] =
            create_font_exact(12, FW_NORMAL, L"Segoe MDL2 Assets");
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
        RECT frame = {0, 0, 0, 0};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                      WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        frame.right = scale_ui(UI_CLIENT_W);
        frame.bottom = scale_ui(UI_CLIENT_H);
        (void)AdjustWindowRectEx(
            &frame, style, FALSE, WS_EX_APPWINDOW | WS_EX_TOPMOST);
        g_app.window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST,
                                       APP_CLASS,
                                       APP_NAME,
                                       style,
                                       CW_USEDEFAULT,
                                       CW_USEDEFAULT,
                                       frame.right - frame.left,
                                       frame.bottom - frame.top,
                                       NULL,
                                       NULL,
                                       instance,
                                       NULL);
    }
    if (g_app.window == NULL) {
        result = 1;
        goto cleanup;
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
    ShowWindow(g_app.window, SW_SHOWNORMAL);
    UpdateWindow(g_app.window);
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
