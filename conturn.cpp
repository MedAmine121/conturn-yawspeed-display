#define _CRT_SECURE_NO_WARNINGS
#include <cwchar>
#include <cstdio>
#include <optional>
#include <algorithm>
#include <utility>
#define NOMINMAX
// clang-format off
#include <windows.h>
// clang-format on
#include <pathcch.h>
#include <shlobj.h>
#include <shlwapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "version.lib")
#pragma comment(lib, "pathcch.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

namespace common {

namespace win32 {

const auto SHELL_TASKBAR_CREATED_MSG = RegisterWindowMessageW(L"TaskbarCreated");

HWND create_window(const wchar_t *class_name, const wchar_t *window_name, WNDPROC proc)
{
    auto instance = GetModuleHandle(nullptr);

    WNDCLASSEXW cls = {};
    cls.cbSize = sizeof(WNDCLASSEX);
    cls.lpfnWndProc = proc;
    cls.hInstance = instance;
    cls.lpszClassName = class_name;
    RegisterClassExW(&cls);

    return CreateWindowExW(0, class_name, window_name, 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
}

void open_folder_and_select(const wchar_t *path)
{
    PIDLIST_ABSOLUTE item;
    SFGAOF sfgaof;
    if (S_OK != SHParseDisplayName(path, nullptr, &item, 0, &sfgaof)) {
        return;
    }
    SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    CoTaskMemFree(item);
}

}

}

using namespace common;

inline constexpr size_t CFG_MAX_COUNT = 8192;
inline constexpr size_t CON_LINE_MAX_COUNT = 8192;
inline constexpr size_t CON_VAR_MAX_COUNT = 256;

struct VersionInfo
{
    wchar_t name[128];
    wchar_t title[128];
    wchar_t version[128];
    wchar_t copyright[128];

    static VersionInfo load(const wchar_t *path)
    {
        VersionInfo info;

        DWORD handle;
        auto size = GetFileVersionInfoSizeW(path, &handle);

        auto buffer = std::malloc(size);

        GetFileVersionInfoW(path, handle, size, buffer);

        UINT count;
        wchar_t *s;

        VerQueryValueW(buffer, L"\\StringFileInfo\\040904E4\\InternalName", reinterpret_cast<void **>(&s), &count);
        std::swprintf(info.name, std::size(info.name), L"%s", s);

        VerQueryValueW(buffer, L"\\StringFileInfo\\040904E4\\ProductName", reinterpret_cast<void **>(&s), &count);
        std::swprintf(info.title, std::size(info.title), L"%s", s);

        VerQueryValueW(buffer, L"\\StringFileInfo\\040904E4\\ProductVersion", reinterpret_cast<void **>(&s), &count);
        std::swprintf(info.version, std::size(info.version), L"%s", s);

        VerQueryValueW(buffer, L"\\StringFileInfo\\040904E4\\LegalCopyright", reinterpret_cast<void **>(&s), &count);
        std::swprintf(info.copyright, std::size(info.copyright), L"%s", s);

        std::free(buffer);

        return info;
    }
};

struct ConLogPipe
{
    static std::optional<ConLogPipe> try_create(const wchar_t *path)
    {
        // https://stackoverflow.com/a/38413449

        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, true, nullptr, false);

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = false;

        auto pipe = CreateNamedPipeW(path, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, sizeof(buffer), sizeof(buffer), 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }

        auto event = CreateEventW(nullptr, true, true, nullptr);
        return ConLogPipe(pipe, event);
    }

    auto event()
    {
        return overlapped.hEvent;
    }

    auto connected()
    {
        return (state == State::READING);
    }

    auto client_pid()
    {
        return client_pid_;
    }

    bool get_next_line(char (&line)[CON_LINE_MAX_COUNT])
    {
    loop:
        if (pending) {
            DWORD transferred;
            auto success = GetOverlappedResult(pipe, &overlapped, &transferred, false);

            if (success) {
                switch (state) {
                case State::CONNECTING:
                    state = State::READING;
                    pending = false;
                    GetNamedPipeClientProcessId(pipe, &client_pid_);
                    goto loop;
                case State::READING:
                    buffer_count += transferred;
                    pending = false;
                    goto loop;
                }
            }

            switch (GetLastError()) {
            case ERROR_IO_INCOMPLETE:
                return false;
            }

            switch (state) {
            case State::CONNECTING:
                pending = false;
                goto loop;
            case State::READING:
                state = State::DISCONNECTING;
                pending = false;
                goto loop;
            }
        }

        switch (state) {
        case State::CONNECTING:
            ConnectNamedPipe(pipe, &overlapped);

            switch (GetLastError()) {
            case ERROR_IO_PENDING:
                pending = true;
                return false;
            case ERROR_PIPE_CONNECTED:
                state = State::READING;
                goto loop;
            default:
                return false;
            }
        case State::READING:
            {
                for (size_t i = 0; i < buffer_count; ++i) {
                    if (buffer[i] == '\n') {
                        std::strncpy(line, buffer, i - 1);
                        line[i - 1] = '\0';

                        std::memmove(buffer, buffer + (i + 1), (std::size(buffer) - (i + 1)) * sizeof(*buffer));
                        buffer_count -= i + 1;

                        return true;
                    }
                }

                DWORD read;
                auto success = ReadFile(pipe, buffer + buffer_count, (std::size(buffer) - buffer_count - 1) * sizeof(*buffer), &read, &overlapped);

                if (success) {
                    buffer_count += read;
                    goto loop;
                }

                switch (GetLastError()) {
                case ERROR_IO_PENDING:
                    pending = true;
                    return false;
                default:
                    state = State::DISCONNECTING;
                    goto loop;
                }
            }
        case State::DISCONNECTING:
            DisconnectNamedPipe(pipe);
            state = State::CONNECTING;
            goto loop;
        }
    }

private:
    ConLogPipe(HANDLE pipe_, HANDLE event)
    {
        pending = false;
        state = State::CONNECTING;
        pipe = pipe_;

        overlapped = {};
        overlapped.hEvent = event;

        buffer_count = 0;
    }

    enum class State
    {
        CONNECTING,
        READING,
        DISCONNECTING
    };

    bool pending;
    State state;
    HANDLE pipe;
    OVERLAPPED overlapped;
    DWORD client_pid_;
    char buffer[CON_LINE_MAX_COUNT];
    size_t buffer_count;
};

struct CtrlSignalHandler
{
    CtrlSignalHandler(HWND hwnd_)
    {
        hwnd = hwnd_;
        event = CreateEventW(nullptr, false, false, nullptr);

        SetConsoleCtrlHandler(proc, true);
    }

    void done()
    {
        SetEvent(event);
    }

private:
    static BOOL proc(DWORD dwCtrlType)
    {
        switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            PostMessageW(hwnd, WM_QUIT, 0, 0);
            WaitForSingleObject(event, INFINITE);
            return true;
        }
        return false;
    }

    static inline HWND hwnd;
    static inline HANDLE event;
};

struct TrayIcon
{
    using CreateMenu_t = HMENU (*)();

    TrayIcon(HWND hwnd, const wchar_t *tip, CreateMenu_t create_menu_)
    {
        data = {};
        data.uVersion = NOTIFYICON_VERSION_4;
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uID = 1;
        data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
        data.uCallbackMessage = WINDOW_MSG;
        data.hIcon = static_cast<HICON>(::LoadImageW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

        create_menu = create_menu_;

        Shell_NotifyIconW(NIM_ADD, &data);
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }

    ~TrayIcon()
    {
        Shell_NotifyIconW(NIM_DELETE, &data);
    }

    void set_tip(const wchar_t *tip)
    {
        std::swprintf(data.szTip, std::size(data.szTip), L"%s", tip);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    bool handle_msg(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        if (uMsg == win32::SHELL_TASKBAR_CREATED_MSG) {
            Shell_NotifyIconW(NIM_ADD, &data);
            Shell_NotifyIconW(NIM_SETVERSION, &data);
            return true;
        }

        switch (uMsg) {
        case WINDOW_MSG:
            switch (LOWORD(lParam)) {
            case WM_CONTEXTMENU:
                {
                    auto menu = create_menu();

                    POINT point;
                    GetCursorPos(&point);

                    SetForegroundWindow(hwnd);
                    TrackPopupMenuEx(menu, TPM_LEFTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN, point.x, point.y, hwnd, nullptr);
                    PostMessageW(hwnd, WM_NULL, 0, 0);

                    DestroyMenu(menu);
                }
                break;
            }
        default:
            return false;
        }
    }

private:
    static constexpr auto WINDOW_MSG = WM_USER + 0;

    NOTIFYICONDATAW data;

    CreateMenu_t create_menu;
};

struct OsdOverlay
{
    OsdOverlay()
    {
        auto instance = GetModuleHandle(nullptr);

        WNDCLASSEXW cls = {};
        cls.cbSize = sizeof(WNDCLASSEX);
        cls.lpfnWndProc = proc;
        cls.hInstance = instance;
        cls.lpszClassName = L"ConturnOSD";
        cls.hbrBackground = nullptr;
        RegisterClassExW(&cls);

        int width = 500;
        int height = 120;
        int x = 40;
        int y = 40;

        hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"ConturnOSD", L"ConturnOSD", WS_POPUP,
            x, y, width, height,
            nullptr, nullptr, instance, nullptr
        );

        if (hwnd) {
            SetLayeredWindowAttributes(hwnd, COLOR_KEY, 0, LWA_COLORKEY);
        }
    }

    ~OsdOverlay()
    {
        if (hwnd) {
            DestroyWindow(hwnd);
        }
    }

    void show(const char *yawspeed_val)
    {
        if (!hwnd) {
            return;
        }

        std::swprintf(text_, std::size(text_), L"%S", yawspeed_val);

        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(hwnd, nullptr, TRUE);

        SetTimer(hwnd, TIMER_ID, 1500, nullptr);
    }

private:
    static constexpr UINT_PTR TIMER_ID = 1001;
    static constexpr COLORREF COLOR_KEY = RGB(1, 1, 1);

    static void draw_outlined_text(HDC hdc, const wchar_t *text, RECT rc, HFONT font)
    {
        auto oldFont = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);

        OffsetRect(&rc, 4, 4);

        SetTextColor(hdc, RGB(0, 0, 0));
        for (int dx = -2; dx <= 2; ++dx) {
            for (int dy = -2; dy <= 2; ++dy) {
                if (dx == 0 && dy == 0) continue;
                RECT offset_rc = rc;
                OffsetRect(&offset_rc, dx, dy);
                DrawTextW(hdc, text, -1, &offset_rc, DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
        }

        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        switch (uMsg) {
        case WM_TIMER:
            if (wParam == TIMER_ID) {
                KillTimer(hwnd, TIMER_ID);
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;

        case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);

                RECT rc;
                GetClientRect(hwnd, &rc);

                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
                HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

                HBRUSH keyBrush = CreateSolidBrush(COLOR_KEY);
                FillRect(memDC, &rc, keyBrush);
                DeleteObject(keyBrush);

                HFONT font = CreateFontW(
                    80, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
                );

                draw_outlined_text(memDC, text_, rc, font);

                DeleteObject(font);

                BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, memDC, 0, 0, SRCCOPY);

                SelectObject(memDC, oldBmp);
                DeleteObject(memBmp);
                DeleteDC(memDC);

                EndPaint(hwnd, &ps);
                return 0;
            }
        case WM_ERASEBKGND:
            return 1;
        }

        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    HWND hwnd = nullptr;
    static inline wchar_t text_[64] = L"";
};

struct ConVar
{
    char name[CON_VAR_MAX_COUNT];
    char value[CON_VAR_MAX_COUNT];

    ConVar(const char *name_)
    {
        std::snprintf(name, std::size(name), "%s", name_);
        std::snprintf(pattern, std::size(pattern), R"("%s" = ")", name_);
    }

    bool parse_con_cvar_line(const char *line)
    {
        if (0 == std::strncmp(line, "[engine] ", 9)) {
            line += 9;
        } else if (line[0] == '[') {
            const char *close_bracket = std::strchr(line, ']');
            if (close_bracket && close_bracket[1] == ' ') {
                line = close_bracket + 2;
            }
        }

        auto start = line;
        for (; pattern[start - line] != '\0'; ++start) {
            if (*start != pattern[start - line]) {
                return false;
            }
        }

        for (size_t i = 0; start[i] != '\0'; ++i) {
            if (start[i] == '"') {
                auto count = std::min(std::size(value), i + 1);
                std::strncpy(value, start, count - 1);
                value[count - 1] = '\0';
                return true;
            }
        }

        return false;
    }

private:
    char pattern[CON_VAR_MAX_COUNT];
};

bool is_momentum_exe(const wchar_t *path)
{
    const wchar_t *filename = PathFindFileNameW(path);
    return (_wcsicmp(filename, L"momentum.exe") == 0);
}

int find_game_steam_appid(const wchar_t *game_path)
{
    if (is_momentum_exe(game_path)) {
        return 1802710;
    }

    wchar_t steam_appid_path[PATHCCH_MAX_CCH];
    std::wcscpy(steam_appid_path, game_path);
    PathCchRemoveFileSpec(steam_appid_path, std::size(steam_appid_path));
    PathCchAppend(steam_appid_path, std::size(steam_appid_path), LR"(steam_appid.txt)");

    char text[32];

    HANDLE file = CreateFileW(steam_appid_path, GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!file) {
        return 0;
    }

    DWORD read;
    if (!ReadFile(file, text, (std::size(text) - 1) * sizeof(*text), &read, nullptr)) {
        CloseHandle(file);
        return 0;
    }

    CloseHandle(file);

    text[read] = '\0';

    return std::atoi(text);
}

enum class Command
{
    ABOUT,
    OPEN_FOLDER,
    OPEN_GAME_FOLDER,
    EXIT
};

struct App
{
    static void run()
    {
        GetModuleFileNameW(nullptr, image_path, std::size(image_path));

        version_info = VersionInfo::load(image_path);

        std::swprintf(pipe_path, std::size(pipe_path), LR"(\\.\pipe\%s-log)", version_info.name);
        std::swprintf(cfg_filename, std::size(cfg_filename), LR"(%s.cfg)", version_info.name);
        std::swprintf(con_log_filename, std::size(con_log_filename), LR"(%s.log)", version_info.name);

        con_log_pipe = ConLogPipe::try_create(pipe_path);
        if (!con_log_pipe) {
            wchar_t text[128];
            std::swprintf(text, std::size(text), L"%s is already running!", version_info.title);
            MessageBoxW(nullptr, text, version_info.title, MB_OK | MB_ICONWARNING);
            return;
        }

        std::wcscpy(ini_path, image_path);
        PathCchRenameExtension(ini_path, std::size(ini_path), LR"(ini)");

        GetPrivateProfileStringW(version_info.name, L"GamePath", L"", game_path, std::size(game_path), ini_path);

        steam_appid = 0;

        if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(game_path)) {
            steam_appid = find_game_steam_appid(game_path);
            switch (steam_appid) {
            case 240:
            case 730:
            case 1802710:
                break;
            default:
                steam_appid = 0;
                break;
            }
        }

        if (!steam_appid) {
            if (!show_game_path_dialog(game_path)) {
                return;
            }

            WritePrivateProfileStringW(version_info.name, L"GamePath", game_path, ini_path);

            steam_appid = find_game_steam_appid(game_path);
            switch (steam_appid) {
            case 240:
            case 730:
            case 1802710:
                break;
            default:
                MessageBoxW(nullptr, L"Unsupported game.", version_info.title, MB_OK | MB_ICONERROR);
                return;
            }
        }

        switch (steam_appid) {
        case 240:
            std::wcscpy(game_name, L"cstrike");
            break;
        case 730:
            std::wcscpy(game_name, L"csgo");
            break;
        case 1802710:
            std::wcscpy(game_name, L"momentum");
            break;
        }

        wchar_t game_dir[PATHCCH_MAX_CCH];
        std::wcscpy(game_dir, game_path);
        PathCchRemoveFileSpec(game_dir, std::size(game_dir));
        if (steam_appid == 1802710) {
            if (_wcsicmp(PathFindFileNameW(game_dir), L"win64") == 0) {
                PathCchRemoveFileSpec(game_dir, std::size(game_dir));
            }
            if (_wcsicmp(PathFindFileNameW(game_dir), L"bin") == 0) {
                PathCchRemoveFileSpec(game_dir, std::size(game_dir));
            }
        }

        std::wcscpy(cfg_path, game_dir);
        switch (steam_appid) {
        case 240:
            PathCchAppend(cfg_path, std::size(cfg_path), LR"(cstrike\cfg)");
            break;
        case 730:
            PathCchAppend(cfg_path, std::size(cfg_path), LR"(csgo\cfg)");
            break;
        case 1802710:
            PathCchAppend(cfg_path, std::size(cfg_path), LR"(momentum\cfg)");
            break;
        }
        PathCchAppend(cfg_path, std::size(cfg_path), cfg_filename);

        std::wcscpy(con_log_path, game_dir);
        switch (steam_appid) {
        case 240:
            PathCchAppend(con_log_path, std::size(con_log_path), LR"(cstrike)");
            break;
        case 730:
            PathCchAppend(con_log_path, std::size(con_log_path), LR"(csgo)");
            break;
        case 1802710:
            PathCchAppend(con_log_path, std::size(con_log_path), LR"(momentum)");
            break;
        }
        PathCchAppend(con_log_path, std::size(con_log_path), con_log_filename);

        yawspeed.emplace("cl_yawspeed");
        yawspeed_alt.emplace("_cl_yawspeed");

        delete_cfg_file();
        delete_con_log_file();
        create_con_log_file();
        create_cfg_file();

        auto hwnd = win32::create_window(version_info.name, version_info.name, window_proc);
        CtrlSignalHandler ctrl_signal_handler(hwnd);
        tray_icon.emplace(hwnd, version_info.title, create_tray_menu);
        osd.emplace();

        HANDLE game_process = nullptr;
        std::optional<bool> last_connected;

        do {
            bool handle_window = false;
            bool handle_con_log_pipe = false;
            bool handle_game_process = false;

            if (!game_process) {
                HANDLE handles[] = {con_log_pipe->event()};
                auto result = MsgWaitForMultipleObjects(std::size(handles), handles, false, INFINITE, QS_ALLINPUT);
                switch (result) {
                case WAIT_OBJECT_0 + 0:
                    handle_con_log_pipe = true;
                    break;
                case WAIT_OBJECT_0 + 1:
                    handle_window = true;
                    break;
                }
            } else {
                HANDLE handles[] = {con_log_pipe->event(), game_process};
                auto result = MsgWaitForMultipleObjects(std::size(handles), handles, false, INFINITE, QS_ALLINPUT);
                switch (result) {
                case WAIT_OBJECT_0 + 0:
                    handle_con_log_pipe = true;
                    break;
                case WAIT_OBJECT_0 + 1:
                    handle_game_process = true;
                    break;
                case WAIT_OBJECT_0 + 2:
                    handle_window = true;
                    break;
                }
            }

            if (handle_window) {
            peek:
                MSG msg;
                if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        break;
                    }
                    DispatchMessage(&msg);
                    goto peek;
                }
            }

            if (handle_con_log_pipe) {
                char line[CON_LINE_MAX_COUNT];
                while (con_log_pipe->get_next_line(line)) {
                    handle_con_line(line);
                }

                bool connected = con_log_pipe->connected();
                if (!last_connected || *last_connected != connected) {
                    if (connected && !game_process) {
                        game_process = OpenProcess(SYNCHRONIZE, false, con_log_pipe->client_pid());
                    }

                    wchar_t buffer[128];
                    if (connected) {
                        std::swprintf(buffer, std::size(buffer), L"%s (%s) [attached: PID %d]", version_info.title, game_name, con_log_pipe->client_pid());
                    } else {
                        std::swprintf(buffer, std::size(buffer), L"%s (%s) [not attached]", version_info.title, game_name);
                    }
                    tray_icon->set_tip(buffer);
                }
                last_connected = connected;
            }

            if (handle_game_process) {
                CloseHandle(game_process);
                game_process = nullptr;

                create_cfg_file();
            }
        } while (true);

        tray_icon.reset();
        osd.reset();

        delete_cfg_file();
        delete_con_log_file();

        ctrl_signal_handler.done();
    }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        switch (uMsg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_COMMAND:
            switch (static_cast<Command>(LOWORD(wParam))) {
            case Command::ABOUT:
                ShellExecuteW(nullptr, nullptr, version_info.copyright, nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case Command::OPEN_FOLDER:
                win32::open_folder_and_select(image_path);
                break;
            case Command::OPEN_GAME_FOLDER:
                win32::open_folder_and_select(game_path);
                break;
            case Command::EXIT:
                PostMessageW(hwnd, WM_QUIT, 0, 0);
                break;
            }
        default:
            if (tray_icon->handle_msg(hwnd, uMsg, wParam, lParam)) {
                return 0;
            }
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        return 0;
    }

    static HMENU create_tray_menu()
    {
        wchar_t buffer[128];

        auto menu = CreatePopupMenu();

        std::swprintf(buffer, std::size(buffer), L"%s %s", version_info.title, version_info.version);
        AppendMenuW(menu, MF_STRING, std::to_underlying(Command::ABOUT), buffer);

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        if (con_log_pipe->connected()) {
            std::swprintf(buffer, std::size(buffer), L"(attached: PID %d)", con_log_pipe->client_pid());
        } else {
            std::swprintf(buffer, std::size(buffer), L"(not attached)");
        }
        AppendMenuW(menu, MF_GRAYED, 0, buffer);

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        std::swprintf(buffer, std::size(buffer), L"Open game folder (%s)...", game_name);
        AppendMenuW(menu, MF_STRING, std::to_underlying(Command::OPEN_GAME_FOLDER), buffer);

        std::swprintf(buffer, std::size(buffer), L"Open %s folder...", version_info.title);
        AppendMenuW(menu, MF_STRING, std::to_underlying(Command::OPEN_FOLDER), buffer);

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(menu, MF_STRING, std::to_underlying(Command::EXIT), L"Exit");

        return menu;
    }

    static bool show_game_path_dialog(wchar_t (&path)[PATHCCH_MAX_CCH])
    {
        OPENFILENAMEW info;
        info.lStructSize = sizeof(OPENFILENAMEW);
        info.hwndOwner = nullptr;
        info.hInstance = nullptr;
        info.lpstrFilter = L"Game .exe file (csgo.exe/hl2.exe/momentum.exe)\0csgo.exe;hl2.exe;momentum.exe\0All Files (*.*)\0*.*\0";
        info.lpstrCustomFilter = nullptr;
        info.nFilterIndex = 0;
        info.lpstrFile = path;
        info.nMaxFile = std::size(path);
        info.lpstrFileTitle = nullptr;
        info.lpstrInitialDir = nullptr;
        info.lpstrTitle = L"Select your game .exe file";
        info.Flags = OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        info.nFileOffset = 0;
        info.nFileExtension = 0;
        info.lpstrDefExt = nullptr;
        info.FlagsEx = 0;
        return GetOpenFileNameW(&info);
    }

    static void create_cfg_file()
    {
        char text[CFG_MAX_COUNT];
        auto count = std::snprintf(text, std::size(text), 1 + R"(
alias %S_off "con_logfile :; con_logfile"

con_logfile %S
cl_yawspeed
_cl_yawspeed
)",
            version_info.name,
            con_log_filename);

        HANDLE file = CreateFileW(cfg_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written;
        WriteFile(file, text, count * sizeof(*text), &written, nullptr);
        CloseHandle(file);
    }

    static void delete_cfg_file()
    {
        DeleteFileW(cfg_path);
    }

    static void create_con_log_file()
    {
        if (!CreateSymbolicLinkW(con_log_path, pipe_path, 0)) {
            auto err = GetLastError();
            if (err == ERROR_PRIVILEGE_NOT_HELD) {
                MessageBoxW(nullptr, L"Failed to create console log symlink. Please run conturn as Administrator.", version_info.title, MB_OK | MB_ICONERROR);
            } else if (err == ERROR_SHARING_VIOLATION || err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
                MessageBoxW(nullptr, L"Failed to create console log symlink because conturn.log is in use or already exists. Please close the game and delete conturn.log first.", version_info.title, MB_OK | MB_ICONERROR);
            }
        }
    }

    static void delete_con_log_file()
    {
        DeleteFileW(con_log_path);
    }

    static void handle_con_line(const char *line)
    {
        if (0 == std::strncmp(line, "[engine] ", 9)) {
            line += 9;
        } else if (line[0] == '[') {
            const char *close_bracket = std::strchr(line, ']');
            if (close_bracket && close_bracket[1] == ' ') {
                line = close_bracket + 2;
            }
        }

        if (yawspeed->parse_con_cvar_line(line)) {
            if (osd) {
                osd->show(yawspeed->value);
            }
        } else if (yawspeed_alt->parse_con_cvar_line(line)) {
            if (osd) {
                osd->show(yawspeed_alt->value);
            }
        }
    }

    inline static wchar_t image_path[PATHCCH_MAX_CCH];
    inline static VersionInfo version_info;
    inline static wchar_t pipe_path[PATHCCH_MAX_CCH];
    inline static wchar_t cfg_filename[PATHCCH_MAX_CCH];
    inline static wchar_t con_log_filename[PATHCCH_MAX_CCH];
    inline static std::optional<ConLogPipe> con_log_pipe;
    inline static wchar_t ini_path[PATHCCH_MAX_CCH];
    inline static wchar_t game_path[PATHCCH_MAX_CCH];
    inline static int steam_appid;
    inline static wchar_t game_name[PATHCCH_MAX_CCH];
    inline static wchar_t cfg_path[PATHCCH_MAX_CCH];
    inline static wchar_t con_log_path[PATHCCH_MAX_CCH];
    inline static std::optional<ConVar> yawspeed;
    inline static std::optional<ConVar> yawspeed_alt;
    inline static std::optional<TrayIcon> tray_icon;
    inline static std::optional<OsdOverlay> osd;
};

int main(int argc, char *argv[])
{
    App::run();
    return 0;
}

#ifdef _MSC_VER
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return main(__argc, __argv);
}
#endif
