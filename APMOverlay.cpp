
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

static const int DEFAULT_WIDTH  = 150;
static const int DEFAULT_HEIGHT = 45;

static const int REFRESH_MS = 1000;

static const int MAX_KEY_REPEATS = 5;

static const UINT WM_TRAYICON = WM_APP + 1;

static const UINT ID_TRAY_CLICKABLE = 1001;
static const UINT ID_TRAY_RESET_APM = 1002;
static const UINT ID_TRAY_RESET_INPUT = 1003;
static const UINT ID_TRAY_EXIT = 1004;


// ============================================================
// GLOBALS
// ============================================================

static HWND hwnd = nullptr;

static HHOOK keyboardHook = nullptr;
static HHOOK mouseHook = nullptr;

static bool clickableMode = false;

static int overlayWidth = DEFAULT_WIDTH;
static int overlayHeight = DEFAULT_HEIGHT;

static std::wstring settingsPath;
static std::wstring inputsPath;

static std::vector<ULONGLONG> actions;

static CRITICAL_SECTION actionCS;

static std::map<DWORD, int> keyRepeatCounts;

static std::map<std::wstring, int> buttonCounts;

static std::vector<std::wstring> timelineLines;


// ============================================================
// TIME
// ============================================================

ULONGLONG NowTick()
{
    return GetTickCount64();
}


std::wstring GetClockTime()
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t buffer[64];

    int hour = st.wHour;
    const wchar_t* ampm = L"AM";

    if (hour >= 12)
        ampm = L"PM";

    int displayHour = hour % 12;

    if (displayHour == 0)
        displayHour = 12;

    swprintf_s(
        buffer,
        L"%02d:%02d:%02d.%03d %s",
        displayHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        ampm
    );

    return buffer;
}


// ============================================================
// PATHS
// ============================================================

std::wstring GetExeDirectory()
{
    wchar_t buffer[MAX_PATH];

    DWORD length =
        GetModuleFileNameW(
            nullptr,
            buffer,
            MAX_PATH
        );

    if (length == 0)
        return L".";

    std::wstring path(
        buffer,
        length
    );

    size_t slash =
        path.find_last_of(
            L"\\/"
        );

    if (
        slash == std::wstring::npos
    )
    {
        return L".";
    }

    return path.substr(
        0,
        slash
    );
}


// ============================================================
// SETTINGS SAVE / LOAD
// ============================================================

void SaveWindowSettings()
{
    if (!hwnd)
        return;

    RECT rc;

    if (
        !GetWindowRect(
            hwnd,
            &rc
        )
    )
    {
        return;
    }

    std::wofstream file(
        settingsPath
    );

    if (!file)
        return;

    file
        << rc.left << L"\n"
        << rc.top << L"\n"
        << (rc.right - rc.left) << L"\n"
        << (rc.bottom - rc.top) << L"\n";
}


void LoadWindowSettings()
{
    std::wifstream file(
        settingsPath
    );

    if (!file)
        return;

    int x;
    int y;
    int width;
    int height;

    if (
        file >>
        x >>
        y >>
        width >>
        height
    )
    {
        if (width >= 100)
            overlayWidth = width;

        if (height >= 30)
            overlayHeight = height;

        SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            x,
            y,
            overlayWidth,
            overlayHeight,
            SWP_NOACTIVATE
        );
    }
}


// ============================================================
// INPUT TEXT
// ============================================================

void WriteTimelineLine(
    const std::wstring& name
)
{
    std::wstring line =
        GetClockTime() +
        L"\t" +
        name;

    timelineLines.push_back(
        line
    );
}


void RewriteInputsFile()
{
    std::wofstream file(
        inputsPath,
        std::ios::trunc
    );

    if (!file)
        return;

    file
        << L"=== BUTTON PRESS COUNTS ===\n\n";

    for (const auto& pair : buttonCounts)
    {
        file
            << pair.first
            << L" = "
            << pair.second
            << L"\n";
    }

    file
        << L"\n=== INPUT TIMELINE ===\n\n";

    for (const std::wstring& line :
         timelineLines)
    {
        file
            << line
            << L"\n";
    }
}


void RecordAction(
    const std::wstring& name
)
{
    EnterCriticalSection(
        &actionCS
    );

    actions.push_back(
        NowTick()
    );

    buttonCounts[name]++;

    WriteTimelineLine(
        name
    );

    RewriteInputsFile();

    LeaveCriticalSection(
        &actionCS
    );
}


void RecordRelease(
    const std::wstring& name
)
{
    EnterCriticalSection(
        &actionCS
    );

    WriteTimelineLine(
        name + L"_UP"
    );

    RewriteInputsFile();

    LeaveCriticalSection(
        &actionCS
    );
}


// ============================================================
// RESET
// ============================================================

void ResetAPM()
{
    EnterCriticalSection(
        &actionCS
    );

    actions.clear();

    LeaveCriticalSection(
        &actionCS
    );

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE
    );
}


void ResetInputTracker()
{
    EnterCriticalSection(
        &actionCS
    );

    actions.clear();
    buttonCounts.clear();
    timelineLines.clear();

    RewriteInputsFile();

    LeaveCriticalSection(
        &actionCS
    );

    keyRepeatCounts.clear();

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE
    );
}


// ============================================================
// APM
// ============================================================

int GetAPM()
{
    ULONGLONG now =
        NowTick();

    ULONGLONG cutoff =
        now >= 60000
        ? now - 60000
        : 0;

    int count = 0;

    EnterCriticalSection(
        &actionCS
    );

    while (
        !actions.empty() &&
        actions.front() < cutoff
    )
    {
        actions.erase(
            actions.begin()
        );
    }

    count =
        static_cast<int>(
            actions.size()
        );

    LeaveCriticalSection(
        &actionCS
    );

    return count;
}


// ============================================================
// KEY NAMES
// ============================================================

std::wstring KeyName(
    DWORD vk
)
{
    switch (vk)
    {
        case VK_SPACE:
            return L"Space";

        case VK_RETURN:
            return L"Enter";

        case VK_TAB:
            return L"Tab";

        case VK_SHIFT:
            return L"Left Shift";

        case VK_LSHIFT:
            return L"Left Shift";

        case VK_RSHIFT:
            return L"Right Shift";

        case VK_CONTROL:
            return L"Left Ctrl";

        case VK_LCONTROL:
            return L"Left Ctrl";

        case VK_RCONTROL:
            return L"Right Ctrl";

        case VK_MENU:
            return L"Left Alt";

        case VK_LMENU:
            return L"Left Alt";

        case VK_RMENU:
            return L"Right Alt";

        case VK_ESCAPE:
            return L"Esc";

        case VK_BACK:
            return L"Backspace";

        case VK_CAPITAL:
            return L"Caps Lock";

        case VK_DELETE:
            return L"Delete";

        case VK_INSERT:
            return L"Insert";

        case VK_HOME:
            return L"Home";

        case VK_END:
            return L"End";

        case VK_PRIOR:
            return L"Page Up";

        case VK_NEXT:
            return L"Page Down";

        case VK_LEFT:
            return L"Left";

        case VK_RIGHT:
            return L"Right";

        case VK_UP:
            return L"Up";

        case VK_DOWN:
            return L"Down";

        default:
            break;
    }

    if (
        vk >= 'A' &&
        vk <= 'Z'
    )
    {
        wchar_t c =
            static_cast<wchar_t>(
                vk
            );

        return std::wstring(
            1,
            c
        );
    }

    if (
        vk >= '0' &&
        vk <= '9'
    )
    {
        wchar_t c =
            static_cast<wchar_t>(
                vk
            );

        return std::wstring(
            1,
            c
        );
    }

    wchar_t buffer[32];

    swprintf_s(
        buffer,
        L"VK_%02X",
        vk
    );

    return buffer;
}


// ============================================================
// KEYBOARD HOOK
// ============================================================

LRESULT CALLBACK KeyboardProc(
    int code,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (
        code == HC_ACTION &&
        lParam != 0
    )
    {
        KBDLLHOOKSTRUCT* info =
            reinterpret_cast<KBDLLHOOKSTRUCT*>(
                lParam
            );

        DWORD vk =
            info->vkCode;

        if (
            wParam == WM_KEYDOWN ||
            wParam == WM_SYSKEYDOWN
        )
        {
            if (
                vk != VK_F8 &&
                vk != VK_F9
            )
            {
                int& repeats =
                    keyRepeatCounts[vk];

                if (repeats < MAX_KEY_REPEATS)
                {
                    repeats++;

                    RecordAction(
                        KeyName(vk)
                    );
                }
            }
        }
        else if (
            wParam == WM_KEYUP ||
            wParam == WM_SYSKEYUP
        )
        {
            std::wstring name =
                KeyName(vk);

            RecordRelease(
                name
            );

            keyRepeatCounts.erase(
                vk
            );
        }
    }

    return CallNextHookEx(
        keyboardHook,
        code,
        wParam,
        lParam
    );
}


// ============================================================
// MOUSE HOOK
// ============================================================

LRESULT CALLBACK MouseProc(
    int code,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (
        code == HC_ACTION &&
        lParam != 0
    )
    {
        std::wstring name;

        switch (wParam)
        {
            case WM_LBUTTONDOWN:
                name = L"LMB";
                break;

            case WM_RBUTTONDOWN:
                name = L"RMB";
                break;

            case WM_MBUTTONDOWN:
                name = L"MMB";
                break;

            case WM_XBUTTONDOWN:
            {
                MSLLHOOKSTRUCT* info =
                    reinterpret_cast<MSLLHOOKSTRUCT*>(
                        lParam
                    );

                WORD button =
                    HIWORD(
                        info->mouseData
                    );

                if (
                    button ==
                    XBUTTON1
                )
                {
                    name = L"X1";
                }
                else
                {
                    name = L"X2";
                }

                break;
            }

            case WM_LBUTTONUP:
                RecordRelease(L"LMB");
                break;

            case WM_RBUTTONUP:
                RecordRelease(L"RMB");
                break;

            case WM_MBUTTONUP:
                RecordRelease(L"MMB");
                break;

            case WM_XBUTTONUP:
            {
                MSLLHOOKSTRUCT* info =
                    reinterpret_cast<MSLLHOOKSTRUCT*>(
                        lParam
                    );

                WORD button =
                    HIWORD(
                        info->mouseData
                    );

                if (
                    button ==
                    XBUTTON1
                )
                {
                    RecordRelease(L"X1");
                }
                else
                {
                    RecordRelease(L"X2");
                }

                break;
            }
        }

        if (!name.empty())
        {
            RecordAction(
                name
            );
        }
    }

    return CallNextHookEx(
        mouseHook,
        code,
        wParam,
        lParam
    );
}


// ============================================================
// TRAY
// ============================================================

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{};

    nid.cbSize =
        sizeof(nid);

    nid.hWnd =
        hwnd;

    nid.uID = 1;

    nid.uFlags =
        NIF_ICON |
        NIF_MESSAGE |
        NIF_TIP;

    nid.uCallbackMessage =
        WM_TRAYICON;

    nid.hIcon =
        LoadIconW(
            nullptr,
            IDI_APPLICATION
        );

    wcscpy_s(
        nid.szTip,
        L"APM Overlay"
    );

    Shell_NotifyIconW(
        NIM_ADD,
        &nid
    );
}


void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid{};

    nid.cbSize =
        sizeof(nid);

    nid.hWnd =
        hwnd;

    nid.uID = 1;

    Shell_NotifyIconW(
        NIM_DELETE,
        &nid
    );
}


void ShowTrayMenu()
{
    HMENU menu =
        CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING |
        (
            clickableMode
            ? MF_CHECKED
            : 0
        ),
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

    POINT pt;

    GetCursorPos(
        &pt
    );

    SetForegroundWindow(
        hwnd
    );

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        pt.x,
        pt.y,
        0,
        hwnd,
        nullptr
    );

    DestroyMenu(
        menu
    );
}


// ============================================================
// WINDOW STYLE
// ============================================================

void ApplyMode()
{
    LONG_PTR style =
        GetWindowLongPtrW(
            hwnd,
            GWL_STYLE
        );

    LONG_PTR exStyle =
        GetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE
        );

    if (clickableMode)
    {
        style |= WS_THICKFRAME;

        exStyle &=
            ~WS_EX_TRANSPARENT;

        exStyle &=
            ~WS_EX_NOACTIVATE;
    }
    else
    {
        style &=
            ~WS_THICKFRAME;

        exStyle |=
            WS_EX_TRANSPARENT;

        exStyle |=
            WS_EX_NOACTIVATE;
    }

    SetWindowLongPtrW(
        hwnd,
        GWL_STYLE,
        style
    );

    SetWindowLongPtrW(
        hwnd,
        GWL_EXSTYLE,
        exStyle
    );

    SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOACTIVATE |
        SWP_FRAMECHANGED
    );

    InvalidateRect(
        hwnd,
        nullptr,
        TRUE
    );
}


void ToggleClickable()
{
    clickableMode =
        !clickableMode;

    ApplyMode();
}


// ============================================================
// PAINT
// ============================================================

HFONT CreateOverlayFont(
    int height
)
{
    return CreateFontW(
        -height,
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
        L"Arial"
    );
}


void PaintOverlay(
    HDC hdc,
    RECT& rc
)
{
    int width =
        rc.right -
        rc.left;

    int height =
        rc.bottom -
        rc.top;

    SetBkMode(
        hdc,
        TRANSPARENT
    );

    HFONT font =
        CreateOverlayFont(
            max(
                18,
                height - 12
            )
        );

    HFONT oldFont =
        (HFONT)SelectObject(
            hdc,
            font
        );

    wchar_t text[64];

    swprintf_s(
        text,
        L"APM %d",
        GetAPM()
    );

    RECT textRect = rc;

    if (clickableMode)
    {
        textRect.top += 6;
    }

    SetTextColor(
        hdc,
        RGB(
            255,
            255,
            255
        )
    );

    DrawTextW(
        hdc,
        text,
        -1,
        &textRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    if (clickableMode)
    {
        RECT bar{
            0,
            0,
            width,
            6
        };

        HBRUSH white =
            CreateSolidBrush(
                RGB(
                    255,
                    255,
                    255
                )
            );

        FillRect(
            hdc,
            &bar,
            white
        );

        DeleteObject(
            white
        );
    }

    SelectObject(
        hdc,
        oldFont
    );

    DeleteObject(
        font
    );
}


// ============================================================
// HIT TEST
// ============================================================

LRESULT HandleHitTest(
    LPARAM lParam
)
{
    if (!clickableMode)
        return HTTRANSPARENT;

    POINT pt{
        GET_X_LPARAM(lParam),
        GET_Y_LPARAM(lParam)
    };

    RECT rc;

    GetClientRect(
        hwnd,
        &rc
    );

    const int grip = 8;

    bool left =
        pt.x < grip;

    bool right =
        pt.x >= rc.right - grip;

    bool top =
        pt.y < grip;

    bool bottom =
        pt.y >= rc.bottom - grip;

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


// ============================================================
// WINDOW PROCEDURE
// ============================================================

LRESULT CALLBACK WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (msg)
    {
        case WM_NCHITTEST:
            return HandleHitTest(
                lParam
            );

        case WM_PAINT:
        {
            PAINTSTRUCT ps;

            HDC hdc =
                BeginPaint(
                    hWnd,
                    &ps
                );

            RECT rc;

            GetClientRect(
                hWnd,
                &rc
            );

            HBRUSH brush =
                CreateSolidBrush(
                    RGB(
                        0,
                        0,
                        0
                    )
                );

            FillRect(
                hdc,
                &rc,
                brush
            );

            DeleteObject(
                brush
            );

            PaintOverlay(
                hdc,
                rc
            );

            EndPaint(
                hWnd,
                &ps
            );

            return 0;
        }

        case WM_TIMER:
        {
            if (wParam == 1)
            {
                InvalidateRect(
                    hWnd,
                    nullptr,
                    FALSE
                );
            }

            return 0;
        }

        case WM_MOVING:
        case WM_SIZING:
        {
            SaveWindowSettings();
            return TRUE;
        }

        case WM_EXITSIZEMOVE:
        {
            SaveWindowSettings();
            return 0;
        }

        case WM_TRAYICON:
        {
            if (
                lParam == WM_RBUTTONUP ||
                lParam == WM_CONTEXTMENU
            )
            {
                ShowTrayMenu();
            }

            return 0;
        }

        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case ID_TRAY_CLICKABLE:
                    ToggleClickable();
                    break;

                case ID_TRAY_RESET_APM:
                    ResetAPM();
                    break;

                case ID_TRAY_RESET_INPUT:
                    ResetInputTracker();
                    break;

                case ID_TRAY_EXIT:
                    DestroyWindow(
                        hWnd
                    );
                    break;
            }

            return 0;
        }

        case WM_CLOSE:
            SaveWindowSettings();

            DestroyWindow(
                hWnd
            );

            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();

            if (keyboardHook)
            {
                UnhookWindowsHookEx(
                    keyboardHook
                );

                keyboardHook =
                    nullptr;
            }

            if (mouseHook)
            {
                UnhookWindowsHookEx(
                    mouseHook
                );

                mouseHook =
                    nullptr;
            }

            KillTimer(
                hWnd,
                1
            );

            PostQuitMessage(
                0
            );

            return 0;
    }

    return DefWindowProcW(
        hWnd,
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
        &actionCS
    );

    std::wstring exeDir =
        GetExeDirectory();

    settingsPath =
        exeDir +
        L"\\APMOverlay.txt";

    inputsPath =
        exeDir +
        L"\\Inputs.txt";

    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.hInstance =
        hInstance;

    wc.lpfnWndProc =
        WndProc;

    wc.lpszClassName =
        L"APMOverlayWindow";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        nullptr;

    RegisterClassExW(
        &wc
    );

    DWORD exStyle =
        WS_EX_LAYERED |
        WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE;

    hwnd =
        CreateWindowExW(
            exStyle,
            wc.lpszClassName,
            L"APM Overlay",
            WS_POPUP |
            WS_THICKFRAME,
            20,
            20,
            overlayWidth,
            overlayHeight,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!hwnd)
    {
        DeleteCriticalSection(
            &actionCS
        );

        return 1;
    }

    SetLayeredWindowAttributes(
        hwnd,
        RGB(
            0,
            0,
            0
        ),
        255,
        LWA_COLORKEY
    );

    LoadWindowSettings();

    ApplyMode();

    ShowWindow(
        hwnd,
        SW_SHOWNOACTIVATE
    );

    UpdateWindow(
        hwnd
    );

    AddTrayIcon();

    SetTimer(
        hwnd,
        1,
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

    if (!keyboardHook ||
        !mouseHook)
    {
        MessageBoxW(
            nullptr,
            L"Could not install the input hooks.",
            L"APM Overlay",
            MB_ICONERROR
        );
    }

    MSG msg;

    while (
        GetMessageW(
            &msg,
            nullptr,
            0,
            0
        ) > 0
    )
    {
        TranslateMessage(
            &msg
        );

        DispatchMessageW(
            &msg
        );
    }

    DeleteCriticalSection(
        &actionCS
    );

    return 0;
}
