#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

static HWND hwnd = nullptr;
static HHOOK keyboardHook = nullptr;
static HHOOK mouseHook = nullptr;

static std::vector<ULONGLONG> actions;
static CRITICAL_SECTION actionLock;

static bool clickable = false;

// Maximum repeated key actions while a key is held.
static const int MAX_KEY_REPEATS = 5;

// Tracks how many times each virtual key has been counted.
static unsigned char keyRepeatCount[256] = {};

static const UINT WM_TRAYICON = WM_USER + 1;

enum
{
    ID_TRAY = 100,
    ID_CLICKABLE = 101,
    ID_RESET = 102,
    ID_EXIT = 103
};


// ============================================================
// REMOVE ACTIONS OLDER THAN 60 SECONDS
// ============================================================

void RemoveOldActionsLocked()
{
    ULONGLONG now = GetTickCount64();
    const ULONGLONG window = 60000;

    ULONGLONG cutoff =
        (now > window) ? now - window : 0;

    auto it =
        std::lower_bound(
            actions.begin(),
            actions.end(),
            cutoff
        );

    actions.erase(
        actions.begin(),
        it
    );
}


void RemoveOldActions()
{
    EnterCriticalSection(&actionLock);

    RemoveOldActionsLocked();

    LeaveCriticalSection(&actionLock);
}


// ============================================================
// RECORD ACTION
// ============================================================

void Action()
{
    EnterCriticalSection(&actionLock);

    actions.push_back(GetTickCount64());

    RemoveOldActionsLocked();

    LeaveCriticalSection(&actionLock);
}


// ============================================================
// CURRENT APM
// ============================================================

int GetAPM()
{
    EnterCriticalSection(&actionLock);

    RemoveOldActionsLocked();

    int result =
        static_cast<int>(
            actions.size()
        );

    LeaveCriticalSection(&actionLock);

    return result;
}


// ============================================================
// RESET
// ============================================================

void ResetAPM()
{
    EnterCriticalSection(&actionLock);

    actions.clear();

    LeaveCriticalSection(&actionLock);

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE
    );
}


// ============================================================
// CLICKABLE MODE
// ============================================================

void SetClickable(bool value)
{
    clickable = value;

    LONG_PTR exStyle =
        GetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE
        );

    LONG_PTR style =
        GetWindowLongPtrW(
            hwnd,
            GWL_STYLE
        );

    if (clickable)
    {
        // Opaque/editable mode.
        exStyle &= ~WS_EX_TRANSPARENT;
        style |= WS_THICKFRAME;
    }
    else
    {
        // Completely transparent background.
        exStyle |= WS_EX_TRANSPARENT;
        style &= ~WS_THICKFRAME;
    }

    SetWindowLongPtrW(
        hwnd,
        GWL_EXSTYLE,
        exStyle
    );

    SetWindowLongPtrW(
        hwnd,
        GWL_STYLE,
        style
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
    SetClickable(!clickable);
}


// ============================================================
// KEYBOARD HOOK
//
// A key can repeat up to 5 times while held.
//
// Example:
//
// E E E E E       = 5 actions
// E E E E E E E   = still 5 actions
//
// Release E -> pressing E again starts at 1.
// This applies independently to every keyboard key.
// ============================================================

LRESULT CALLBACK KeyboardProc(
    int code,
    WPARAM wParam,
    LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* k =
            reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (k != nullptr)
        {
            UINT vk = k->vkCode;

            if (vk < 256)
            {
                // Key is being pressed.
                if (wParam == WM_KEYDOWN ||
                    wParam == WM_SYSKEYDOWN)
                {
                    // Ignore F8/F9.
                    if (vk != VK_F8 &&
                        vk != VK_F9)
                    {
                        if (keyRepeatCount[vk] <
                            MAX_KEY_REPEATS)
                        {
                            keyRepeatCount[vk]++;

                            Action();
                        }
                    }
                }

                // Key released.
                else if (wParam == WM_KEYUP ||
                         wParam == WM_SYSKEYUP)
                {
                    keyRepeatCount[vk] = 0;
                }
            }
        }
    }

    // NEVER block the keyboard.
    return CallNextHookEx(
        nullptr,
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
    LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        if (wParam == WM_LBUTTONDOWN ||
            wParam == WM_RBUTTONDOWN ||
            wParam == WM_MBUTTONDOWN ||
            wParam == WM_XBUTTONDOWN)
        {
            Action();
        }
    }

    // Always pass mouse events through.
    return CallNextHookEx(
        nullptr,
        code,
        wParam,
        lParam
    );
}


// ============================================================
// TRAY MENU
// ============================================================

void ShowTrayMenu()
{
    POINT p{};

    GetCursorPos(&p);

    HMENU menu =
        CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING |
        (clickable ? MF_CHECKED : 0),
        ID_CLICKABLE,
        L"Clickable Mode"
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
        ID_RESET,
        L"Reset APM"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_EXIT,
        L"Exit"
    );

    SetForegroundWindow(hwnd);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        p.x,
        p.y,
        0,
        hwnd,
        nullptr
    );

    DestroyMenu(menu);
}


// ============================================================
// TRAY ICON
// ============================================================

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{};

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY;

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

    lstrcpyW(
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

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY;

    Shell_NotifyIconW(
        NIM_DELETE,
        &nid
    );
}


// ============================================================
// WINDOW PROCEDURE
// ============================================================

LRESULT CALLBACK WindowProc(
    HWND h,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
        // ----------------------------------------------------
        // Tray
        // ----------------------------------------------------

        case WM_TRAYICON:

            if (lParam == WM_RBUTTONUP)
                ShowTrayMenu();

            return 0;


        // ----------------------------------------------------
        // Menu
        // ----------------------------------------------------

        case WM_COMMAND:

            switch (LOWORD(wParam))
            {
                case ID_CLICKABLE:
                    ToggleClickable();
                    break;

                case ID_RESET:
                    ResetAPM();
                    break;

                case ID_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }

            return 0;


        // ----------------------------------------------------
        // Update every 5 seconds
        // ----------------------------------------------------

        case WM_TIMER:

            RemoveOldActions();

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;


        // ----------------------------------------------------
        // Click-through / clickable
        // ----------------------------------------------------

        case WM_NCHITTEST:

            if (!clickable)
                return HTTRANSPARENT;

            return DefWindowProcW(
                h,
                msg,
                wParam,
                lParam
            );


        // ----------------------------------------------------
        // Clickable Mode
        //
        // Top 10 pixels = resize
        // Rest = move
        // ----------------------------------------------------

        case WM_LBUTTONDOWN:

            if (clickable)
            {
                POINT p{};

                GetCursorPos(&p);

                RECT r{};

                GetWindowRect(
                    hwnd,
                    &r
                );

                if (p.y < r.top + 10)
                {
                    ReleaseCapture();

                    SendMessageW(
                        hwnd,
                        WM_NCLBUTTONDOWN,
                        HTTOP,
                        0
                    );
                }
                else
                {
                    ReleaseCapture();

                    SendMessageW(
                        hwnd,
                        WM_NCLBUTTONDOWN,
                        HTCAPTION,
                        0
                    );
                }

                return 0;
            }

            return 0;


        // ----------------------------------------------------
        // Cursor
        // ----------------------------------------------------

        case WM_SETCURSOR:

            if (clickable)
            {
                POINT p{};

                GetCursorPos(&p);

                RECT r{};

                GetWindowRect(
                    hwnd,
                    &r
                );

                if (p.y < r.top + 10)
                {
                    SetCursor(
                        LoadCursorW(
                            nullptr,
                            IDC_SIZENS
                        )
                    );

                    return TRUE;
                }

                SetCursor(
                    LoadCursorW(
                        nullptr,
                        IDC_SIZEALL
                    )
                );

                return TRUE;
            }

            break;


        // ----------------------------------------------------
        // Paint
        // ----------------------------------------------------

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            HDC dc =
                BeginPaint(
                    hwnd,
                    &ps
                );

            RECT r{};

            GetClientRect(
                hwnd,
                &r
            );


            // ------------------------------------------------
            // Clickable Mode = opaque background
            // Normal Mode = transparent background
            // ------------------------------------------------

            if (clickable)
            {
                HBRUSH bg =
                    CreateSolidBrush(
                        RGB(20, 20, 20)
                    );

                FillRect(
                    dc,
                    &r,
                    bg
                );

                DeleteObject(bg);
            }


            SetBkMode(
                dc,
                TRANSPARENT
            );

            SetTextColor(
                dc,
                RGB(255, 255, 255)
            );


            // ------------------------------------------------
            // Scale font with window height.
            // ------------------------------------------------

            int width =
                r.right - r.left;

            int height =
                r.bottom - r.top;

            int fontHeight =
                height * 60 / 100;

            if (fontHeight < 8)
                fontHeight = 8;

            if (fontHeight > 200)
                fontHeight = 200;


            // ------------------------------------------------
            // Use Radiance Sans
            // ------------------------------------------------

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
                    CLEARTYPE_QUALITY,
                    DEFAULT_PITCH,
                    L"Radiance Sans"
                );


            HFONT old =
                reinterpret_cast<HFONT>(
                    SelectObject(
                        dc,
                        font
                    )
                );


            // ONLY APM

            std::wstring text =
                L"APM " +
                std::to_wstring(
                    GetAPM()
                );


            DrawTextW(
                dc,
                text.c_str(),
                -1,
                &r,
                DT_CENTER |
                DT_VCENTER |
                DT_SINGLELINE |
                DT_NOPREFIX
            );


            SelectObject(
                dc,
                old
            );

            DeleteObject(font);


            EndPaint(
                hwnd,
                &ps
            );

            return 0;
        }


        // ----------------------------------------------------
        // Destroy
        // ----------------------------------------------------

        case WM_DESTROY:

            KillTimer(
                hwnd,
                1
            );

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

            DeleteCriticalSection(
                &actionLock
            );

            PostQuitMessage(0);

            return 0;
    }

    return DefWindowProcW(
        h,
        msg,
        wParam,
        lParam
    );
}


// ============================================================
// MAIN
// ============================================================

int WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE,
    LPSTR,
    int)
{
    InitializeCriticalSection(
        &actionLock
    );


    // --------------------------------------------------------
    // Load Radiance Sans from the EXE's folder.
    // --------------------------------------------------------

    wchar_t exePath[MAX_PATH]{};

    GetModuleFileNameW(
        nullptr,
        exePath,
        MAX_PATH
    );

    std::wstring path(exePath);

    size_t slash =
        path.find_last_of(
            L"\\/"
        );

    if (slash != std::wstring::npos)
    {
        path.resize(
            slash + 1
        );
    }

    path += L"Radiance_Sans.ttf";


    // Add the font temporarily for this process.
    AddFontResourceExW(
        path.c_str(),
        FR_PRIVATE,
        nullptr
    );


    WNDCLASSW wc{};

    wc.hInstance =
        instance;

    wc.lpfnWndProc =
        WindowProc;

    wc.lpszClassName =
        L"APMOverlay";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    RegisterClassW(&wc);


    hwnd =
        CreateWindowExW(
            WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW |
            WS_EX_LAYERED |
            WS_EX_TRANSPARENT |
            WS_EX_NOACTIVATE,

            L"APMOverlay",
            L"APM",

            WS_POPUP,

            20,
            20,
            150,
            45,

            nullptr,
            nullptr,
            instance,
            nullptr
        );


    if (!hwnd)
    {
        RemoveFontResourceExW(
            path.c_str(),
            FR_PRIVATE,
            nullptr
        );

        DeleteCriticalSection(
            &actionLock
        );

        return 1;
    }


    // Alpha is only used for the text/window surface.
    // Normal mode remains visually transparent.
    SetLayeredWindowAttributes(
        hwnd,
        0,
        255,
        LWA_ALPHA
    );


    keyboardHook =
        SetWindowsHookExW(
            WH_KEYBOARD_LL,
            KeyboardProc,
            instance,
            0
        );

    mouseHook =
        SetWindowsHookExW(
            WH_MOUSE_LL,
            MouseProc,
            instance,
            0
        );


    if (!keyboardHook ||
        !mouseHook)
    {
        MessageBoxW(
            nullptr,
            L"Could not install input hooks.",
            L"APM Overlay",
            MB_ICONERROR
        );

        if (keyboardHook)
            UnhookWindowsHookEx(
                keyboardHook
            );

        if (mouseHook)
            UnhookWindowsHookEx(
                mouseHook
            );

        RemoveFontResourceExW(
            path.c_str(),
            FR_PRIVATE,
            nullptr
        );

        DeleteCriticalSection(
            &actionLock
        );

        return 1;
    }


    AddTrayIcon();


    // Display updates every 5 seconds.
    // APM remains a rolling 60-second window.

    SetTimer(
        hwnd,
        1,
        5000,
        nullptr
    );


    ShowWindow(
        hwnd,
        SW_SHOWNOACTIVATE
    );


    MSG msg{};

    while (
        GetMessageW(
            &msg,
            nullptr,
            0,
            0
        )
    )
    {
        TranslateMessage(&msg);

        DispatchMessageW(
            &msg
        );
    }


    RemoveFontResourceExW(
        path.c_str(),
        FR_PRIVATE,
        nullptr
    );


    return 0;
}
