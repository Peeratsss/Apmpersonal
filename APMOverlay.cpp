#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <map>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

// ============================================================
// SETTINGS
// ============================================================

static const int DEFAULT_WIDTH = 150;
static const int DEFAULT_HEIGHT = 45;

static const UINT TIMER_REFRESH = 1;
static const UINT REFRESH_MS = 1000;

static const int MAX_KEY_REPEATS = 5;

static const UINT WM_TRAYICON = WM_APP + 1;

static const UINT ID_TRAY_CLICKABLE = 1001;
static const UINT ID_TRAY_RESET_APM = 1002;
static const UINT ID_TRAY_RESET_INPUT = 1003;
static const UINT ID_TRAY_EXIT = 1004;

// ============================================================
// GLOBALS
// ============================================================

static HWND hwndOverlay = nullptr;
static HHOOK keyboardHook = nullptr;
static HHOOK mouseHook = nullptr;

static bool clickableMode = false;

static int overlayWidth = DEFAULT_WIDTH;
static int overlayHeight = DEFAULT_HEIGHT;

static int overlayX = 20;
static int overlayY = 20;

static std::vector<ULONGLONG> actions;

static CRITICAL_SECTION actionLock;

static std::map<DWORD, int> keyRepeatCounts;
static std::map<std::wstring, int> buttonCounts;

static std::vector<std::wstring> timelineLines;

// ============================================================
// FILE PATH
// ============================================================

static std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH]{};

    GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::wstring result(path);

    size_t slash = result.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        result.resize(slash);

    return result;
}

static std::wstring GetSettingsPath()
{
    return GetExeDirectory() + L"\\APMOverlay.txt";
}

static std::wstring GetInputsPath()
{
    return GetExeDirectory() + L"\\Inputs.txt";
}

// ============================================================
// TIME
// ============================================================

static std::wstring GetClockTime()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    int hour = st.wHour;
    const wchar_t* ampm = L"AM";

    if (hour >= 12)
    {
        ampm = L"PM";

        if (hour > 12)
            hour -= 12;
    }
    else if (hour == 0)
    {
        hour = 12;
    }

    wchar_t buffer[64];

    swprintf_s(
        buffer,
        L"%02d:%02d:%02d.%03d %s",
        hour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        ampm
    );

    return buffer;
}

// ============================================================
// SETTINGS
// ============================================================

static void LoadSettings()
{
    std::wifstream file(GetSettingsPath());

    if (!file)
        return;

    std::wstring key;

    while (file >> key)
    {
        if (key == L"X")
            file >> overlayX;
        else if (key == L"Y")
            file >> overlayY;
        else if (key == L"WIDTH")
            file >> overlayWidth;
        else if (key == L"HEIGHT")
            file >> overlayHeight;
    }

    if (overlayWidth < 80)
        overlayWidth = DEFAULT_WIDTH;

    if (overlayHeight < 30)
        overlayHeight = DEFAULT_HEIGHT;
}

static void SaveSettings()
{
    std::wofstream file(GetSettingsPath());

    if (!file)
        return;

    file << L"X " << overlayX << L"\n";
    file << L"Y " << overlayY << L"\n";
    file << L"WIDTH " << overlayWidth << L"\n";
    file << L"HEIGHT " << overlayHeight << L"\n";
}

// ============================================================
// INPUT FILE
// ============================================================

static void SaveInputsFile()
{
    std::wofstream file(GetInputsPath());

    if (!file)
        return;

    file << L"=== BUTTON PRESS COUNTS ===\n\n";

    for (const auto& pair : buttonCounts)
    {
        file << pair.first << L" = " << pair.second << L"\n";
    }

    file << L"\n=== INPUT TIMELINE ===\n\n";

    for (const auto& line : timelineLines)
    {
        file << line << L"\n";
    }
}

// ============================================================
// RESET
// ============================================================

static void ResetAPM()
{
    EnterCriticalSection(&actionLock);

    actions.clear();

    LeaveCriticalSection(&actionLock);

    InvalidateRect(hwndOverlay, nullptr, TRUE);
}

static void ResetInputTracker()
{
    EnterCriticalSection(&actionLock);

    actions.clear();
    buttonCounts.clear();
    timelineLines.clear();
    keyRepeatCounts.clear();

    LeaveCriticalSection(&actionLock);

    SaveInputsFile();

    InvalidateRect(hwndOverlay, nullptr, TRUE);
}

// ============================================================
// APM
// ============================================================

static int GetCurrentAPM()
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG windowStart =
        (now >= 60000ULL) ? now - 60000ULL : 0;

    EnterCriticalSection(&actionLock);

    size_t firstValid = 0;

    while (firstValid < actions.size() &&
           actions[firstValid] < windowStart)
    {
        ++firstValid;
    }

    if (firstValid > 0)
    {
        actions.erase(
            actions.begin(),
            actions.begin() + firstValid
        );
    }

    int result = static_cast<int>(actions.size());

    LeaveCriticalSection(&actionLock);

    return result;
}

// ============================================================
// RECORD INPUT
// ============================================================

static void RecordInput(
    const std::wstring& name,
    bool isRelease
)
{
    const std::wstring timestamp = GetClockTime();

    std::wstring line = timestamp;
    line += L"\t";
    line += name;

    if (isRelease)
        line += L"_UP";

    EnterCriticalSection(&actionLock);

    timelineLines.push_back(line);

    if (!isRelease)
    {
        actions.push_back(GetTickCount64());

        buttonCounts[name]++;
    }

    LeaveCriticalSection(&actionLock);

    SaveInputsFile();

    InvalidateRect(hwndOverlay, nullptr, TRUE);
}

// ============================================================
// KEY NAMES
// ============================================================

static std::wstring VirtualKeyToName(DWORD vk)
{
    switch (vk)
    {
        case VK_SPACE: return L"Space";
        case VK_RETURN: return L"Enter";
        case VK_TAB: return L"Tab";
        case VK_ESCAPE: return L"Esc";
        case VK_BACK: return L"Backspace";

        case VK_LSHIFT: return L"Left Shift";
        case VK_RSHIFT: return L"Right Shift";

        case VK_LCONTROL: return L"Left Ctrl";
        case VK_RCONTROL: return L"Right Ctrl";

        case VK_LMENU: return L"Left Alt";
        case VK_RMENU: return L"Right Alt";

        case VK_CAPITAL: return L"Caps Lock";

        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";

        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";

        case VK_F1: return L"F1";
        case VK_F2: return L"F2";
        case VK_F3: return L"F3";
        case VK_F4: return L"F4";
        case VK_F5: return L"F5";
        case VK_F6: return L"F6";
        case VK_F7: return L"F7";
        case VK_F8: return L"F8";
        case VK_F9: return L"F9";
        case VK_F10: return L"F10";
        case VK_F11: return L"F11";
        case VK_F12: return L"F12";

        case VK_NUMPAD0: return L"Num 0";
        case VK_NUMPAD1: return L"Num 1";
        case VK_NUMPAD2: return L"Num 2";
        case VK_NUMPAD3: return L"Num 3";
        case VK_NUMPAD4: return L"Num 4";
        case VK_NUMPAD5: return L"Num 5";
        case VK_NUMPAD6: return L"Num 6";
        case VK_NUMPAD7: return L"Num 7";
        case VK_NUMPAD8: return L"Num 8";
        case VK_NUMPAD9: return L"Num 9";
    }

    if ((vk >= 'A' && vk <= 'Z') ||
        (vk >= '0' && vk <= '9'))
    {
        wchar_t buffer[2]{};

        buffer[0] = static_cast<wchar_t>(vk);

        return buffer;
    }

    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);

    if (scanCode == 0)
        return L"Unknown";

    LONG lParam = static_cast<LONG>(scanCode << 16);

    wchar_t name[128]{};

    if (GetKeyNameTextW(lParam, name, 128) > 0)
        return name;

    return L"Unknown";
}

// ============================================================
// KEYBOARD HOOK
// ============================================================

static LRESULT CALLBACK KeyboardProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (nCode == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* info =
            reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (info)
        {
            DWORD vk = info->vkCode;

            if (vk != VK_F8 && vk != VK_F9)
            {
                if (wParam == WM_KEYDOWN ||
                    wParam == WM_SYSKEYDOWN)
                {
                    int& repeats = keyRepeatCounts[vk];

                    if (repeats < MAX_KEY_REPEATS)
                    {
                        repeats++;

                        RecordInput(
                            VirtualKeyToName(vk),
                            false
                        );
                    }
                }
                else if (wParam == WM_KEYUP ||
                         wParam == WM_SYSKEYUP)
                {
                    keyRepeatCounts[vk] = 0;

                    RecordInput(
                        VirtualKeyToName(vk),
                        true
                    );
                }
            }
        }
    }

    return CallNextHookEx(
        keyboardHook,
        nCode,
        wParam,
        lParam
    );
}

// ============================================================
// MOUSE HOOK
// ============================================================

static std::wstring MouseButtonName(DWORD message)
{
    switch (message)
    {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return L"LMB";

        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return L"RMB";

        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return L"MMB";

        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            return L"XMB";
    }

    return L"Mouse";
}

static LRESULT CALLBACK MouseProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (nCode == HC_ACTION)
    {
        const MSLLHOOKSTRUCT* info =
            reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        if (info)
        {
            switch (wParam)
            {
                case WM_LBUTTONDOWN:
                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN:
                    RecordInput(
                        MouseButtonName(
                            static_cast<DWORD>(wParam)
                        ),
                        false
                    );
                    break;

                case WM_LBUTTONUP:
                case WM_RBUTTONUP:
                case WM_MBUTTONUP:
                    RecordInput(
                        MouseButtonName(
                            static_cast<DWORD>(wParam)
                        ),
                        true
                    );
                    break;

                case WM_XBUTTONDOWN:
                {
                    WORD button =
                        HIWORD(info->mouseData);

                    std::wstring name =
                        (button == XBUTTON1)
                        ? L"X1"
                        : L"X2";

                    RecordInput(name, false);
                    break;
                }

                case WM_XBUTTONUP:
                {
                    WORD button =
                        HIWORD(info->mouseData);

                    std::wstring name =
                        (button == XBUTTON1)
                        ? L"X1"
                        : L"X2";

                    RecordInput(name, true);
                    break;
                }
            }
        }
    }

    return CallNextHookEx(
        mouseHook,
        nCode,
        wParam,
        lParam
    );
}

// ============================================================
// TRAY
// ============================================================

static NOTIFYICONDATAW trayIcon{};

static void AddTrayIcon()
{
    ZeroMemory(&trayIcon, sizeof(trayIcon));

    trayIcon.cbSize = sizeof(trayIcon);
    trayIcon.hWnd = hwndOverlay;
    trayIcon.uID = 1;
    trayIcon.uFlags =
        NIF_MESSAGE |
        NIF_ICON |
        NIF_TIP;

    trayIcon.uCallbackMessage = WM_TRAYICON;
    trayIcon.hIcon =
        LoadIconW(nullptr, IDI_APPLICATION);

    wcscpy_s(
        trayIcon.szTip,
        L"APM Overlay"
    );

    Shell_NotifyIconW(
        NIM_ADD,
        &trayIcon
    );
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(
        NIM_DELETE,
        &trayIcon
    );
}

static void ShowTrayMenu()
{
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING |
        (clickableMode ? MF_CHECKED : 0),
        ID_TRAY_CLICKABLE,
        clickableMode
            ? L"Pass-through Mode"
            : L"Clickable Mode"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_RESET_APM,
        L"Reset APM"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_RESET_INPUT,
        L"Reset Input Tracker"
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_EXIT,
        L"Exit"
    );

    SetForegroundWindow(hwndOverlay);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        pt.x,
        pt.y,
        0,
        hwndOverlay,
        nullptr
    );

    DestroyMenu(menu);
}

// ============================================================
// CLICKABLE MODE
// ============================================================

static void ApplyWindowMode()
{
    LONG_PTR exStyle =
        GetWindowLongPtrW(
            hwndOverlay,
            GWL_EXSTYLE
        );

    LONG_PTR style =
        GetWindowLongPtrW(
            hwndOverlay,
            GWL_STYLE
        );

    if (clickableMode)
    {
        exStyle &= ~WS_EX_TRANSPARENT;

        style |= WS_THICKFRAME;
    }
    else
    {
        exStyle |= WS_EX_TRANSPARENT;

        style &= ~WS_THICKFRAME;
    }

    SetWindowLongPtrW(
        hwndOverlay,
        GWL_EXSTYLE,
        exStyle
    );

    SetWindowLongPtrW(
        hwndOverlay,
        GWL_STYLE,
        style
    );

    SetWindowPos(
        hwndOverlay,
        HWND_TOPMOST,
        overlayX,
        overlayY,
        overlayWidth,
        overlayHeight,
        SWP_FRAMECHANGED |
        SWP_SHOWWINDOW
    );

    InvalidateRect(
        hwndOverlay,
        nullptr,
        TRUE
    );
}

// ============================================================
// DRAWING
// ============================================================

static void PaintOverlay(
    HWND hwnd,
    HDC hdc
)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);

    HBRUSH background =
        CreateSolidBrush(RGB(0, 0, 0));

    FillRect(
        hdc,
        &rc,
        background
    );

    DeleteObject(background);

    // White resize bar in clickable mode
    if (clickableMode)
    {
        RECT bar{
            0,
            0,
            rc.right,
            6
        };

        HBRUSH white =
            CreateSolidBrush(
                RGB(255, 255, 255)
            );

        FillRect(
            hdc,
            &bar,
            white
        );

        DeleteObject(white);
    }

    SetBkMode(
        hdc,
        TRANSPARENT
    );

    SetTextColor(
        hdc,
        RGB(255, 255, 255)
    );

    int fontHeight =
        max(16, rc.bottom - 14);

    HFONT font =
        CreateFontW(
            -fontHeight,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Arial"
        );

    HFONT oldFont =
        static_cast<HFONT>(
            SelectObject(
                hdc,
                font
            )
        );

    wchar_t text[64];

    swprintf_s(
        text,
        L"APM %d",
        GetCurrentAPM()
    );

    RECT textRect{
        0,
        clickableMode ? 6 : 0,
        rc.right,
        rc.bottom
    };

    DrawTextW(
        hdc,
        text,
        -1,
        &textRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(
        hdc,
        oldFont
    );

    DeleteObject(font);
}

// ============================================================
// WINDOW PROCEDURE
// ============================================================

static LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            HDC hdc =
                BeginPaint(
                    hwnd,
                    &ps
                );

            PaintOverlay(
                hwnd,
                hdc
            );

            EndPaint(
                hwnd,
                &ps
            );

            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
        {
            if (wParam == TIMER_REFRESH)
            {
                InvalidateRect(
                    hwnd,
                    nullptr,
                    TRUE
                );
            }

            return 0;
        }

        case WM_MOVING:
        {
            RECT* r =
                reinterpret_cast<RECT*>(lParam);

            if (r)
            {
                overlayX = r->left;
                overlayY = r->top;
            }

            return TRUE;
        }

        case WM_SIZING:
        {
            RECT* r =
                reinterpret_cast<RECT*>(lParam);

            if (r)
            {
                overlayWidth =
                    r->right - r->left;

                overlayHeight =
                    r->bottom - r->top;
            }

            return TRUE;
        }

        case WM_EXITSIZEMOVE:
        {
            RECT r{};

            GetWindowRect(
                hwnd,
                &r
            );

            overlayX = r.left;
            overlayY = r.top;

            overlayWidth =
                r.right - r.left;

            overlayHeight =
                r.bottom - r.top;

            SaveSettings();

            return 0;
        }

        case WM_NCHITTEST:
        {
            if (!clickableMode)
                return HTTRANSPARENT;

            POINT pt{
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)
            };

            ScreenToClient(
                hwnd,
                &pt
            );

            RECT rc{};
            GetClientRect(hwnd, &rc);

            const int grip = 8;

            bool left = pt.x < grip;
            bool right = pt.x >= rc.right - grip;
            bool top = pt.y < grip;
            bool bottom = pt.y >= rc.bottom - grip;

            if (top && left)
                return HTTOPLEFT;

            if (top && right)
                return HTTOPRIGHT;

            if (bottom && left)
                return HTBOTTOMLEFT;

            if (bottom && right)
                return HTBOTTOMRIGHT;

            if (left)
                return HTLEFT;

            if (right)
                return HTRIGHT;

            if (top)
                return HTTOP;

            if (bottom)
                return HTBOTTOM;

            return HTCAPTION;
        }

        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case ID_TRAY_CLICKABLE:
                {
                    RECT r{};

                    GetWindowRect(
                        hwnd,
                        &r
                    );

                    overlayX = r.left;
                    overlayY = r.top;

                    overlayWidth =
                        r.right - r.left;

                    overlayHeight =
                        r.bottom - r.top;

                    clickableMode =
                        !clickableMode;

                    ApplyWindowMode();

                    SaveSettings();

                    return 0;
                }

                case ID_TRAY_RESET_APM:
                    ResetAPM();
                    return 0;

                case ID_TRAY_RESET_INPUT:
                    ResetInputTracker();
                    return 0;

                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
            }

            break;
        }

        case WM_TRAYICON:
        {
            if (lParam == WM_RBUTTONUP)
            {
                ShowTrayMenu();
                return 0;
            }

            if (lParam == WM_LBUTTONDBLCLK)
            {
                clickableMode =
                    !clickableMode;

                ApplyWindowMode();

                return 0;
            }

            break;
        }

        case WM_DESTROY:
        {
            RECT r{};

            GetWindowRect(
                hwnd,
                &r
            );

            overlayX = r.left;
            overlayY = r.top;

            overlayWidth =
                r.right - r.left;

            overlayHeight =
                r.bottom - r.top;

            SaveSettings();

            RemoveTrayIcon();

            if (keyboardHook)
            {
                UnhookWindowsHookEx(
                    keyboardHook
                );

                keyboardHook = nullptr;
            }

            if (mouseHook)
            {
                UnhookWindowsHookEx(
                    mouseHook
                );

                mouseHook = nullptr;
            }

            KillTimer(
                hwnd,
                TIMER_REFRESH
            );

            PostQuitMessage(0);

            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam
    );
}

// ============================================================
// ENTRY POINT
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int
)
{
    InitializeCriticalSection(
        &actionLock
    );

    LoadSettings();

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WindowProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        L"APMOverlayWindow";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        static_cast<HBRUSH>(
            GetStockObject(BLACK_BRUSH)
        );

    RegisterClassW(&wc);

    DWORD exStyle =
        WS_EX_LAYERED |
        WS_EX_TRANSPARENT |
        WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE;

    DWORD style =
        WS_POPUP;

    hwndOverlay =
        CreateWindowExW(
            exStyle,
            wc.lpszClassName,
            L"APM Overlay",
            style,
            overlayX,
            overlayY,
            overlayWidth,
            overlayHeight,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!hwndOverlay)
    {
        DeleteCriticalSection(
            &actionLock
        );

        return 1;
    }

    SetLayeredWindowAttributes(
        hwndOverlay,
        RGB(0, 0, 0),
        0,
        LWA_COLORKEY
    );

    ShowWindow(
        hwndOverlay,
        SW_SHOWNOACTIVATE
    );

    UpdateWindow(hwndOverlay);

    AddTrayIcon();

    SetTimer(
        hwndOverlay,
        TIMER_REFRESH,
        REFRESH_MS,
        nullptr
    );

    keyboardHook =
        SetWindowsHookExW(
            WH_KEYBOARD_LL,
            KeyboardProc,
            hInstance,
            0
        );

    mouseHook =
        SetWindowsHookExW(
            WH_MOUSE_LL,
            MouseProc,
            hInstance,
            0
        );

    SaveInputsFile();

    MSG msg{};

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0
    ) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteCriticalSection(
        &actionLock
    );

    return static_cast<int>(
        msg.wParam
    );
}
