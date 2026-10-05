#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <array>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

static HWND hwnd = nullptr;
static HHOOK keyboardHook = nullptr;
static HHOOK mouseHook = nullptr;

static std::array<int, 60> buckets{};
static int bucket = 0;
static long long total = 0;
static ULONGLONG lastSecond = 0;

static bool clickable = false;

enum
{
    ID_TRAY = 100,
    ID_CLICKABLE = 101,
    ID_RESET = 102,
    ID_EXIT = 103
};

static const UINT WM_TRAYICON = WM_USER + 1;


// ============================================================
// APM
// ============================================================

void Advance()
{
    ULONGLONG now = GetTickCount64() / 1000;

    if (!lastSecond)
    {
        lastSecond = now;
        return;
    }

    ULONGLONG diff = now - lastSecond;

    if (!diff)
        return;

    if (diff >= 60)
    {
        buckets.fill(0);
        total = 0;
        bucket = 0;
    }
    else
    {
        for (ULONGLONG i = 0; i < diff; ++i)
        {
            bucket = (bucket + 1) % 60;
            total -= buckets[bucket];
            buckets[bucket] = 0;
        }
    }

    lastSecond = now;
}


void Action()
{
    Advance();

    buckets[bucket]++;
    total++;
}


void ResetAPM()
{
    buckets.fill(0);

    total = 0;
    bucket = 0;

    lastSecond = GetTickCount64() / 1000;

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
        // Allow interaction
        exStyle &= ~WS_EX_TRANSPARENT;

        // Enable normal resize border
        style |= WS_THICKFRAME;
    }
    else
    {
        // Make mouse pass through
        exStyle |= WS_EX_TRANSPARENT;

        // Remove resize border
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
// ============================================================

LRESULT CALLBACK KeyboardProc(
    int code,
    WPARAM wParam,
    LPARAM lParam)
{
    if (code >= 0)
    {
        if (wParam == WM_KEYDOWN ||
            wParam == WM_SYSKEYDOWN)
        {
            KBDLLHOOKSTRUCT* k =
                reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

            if (k->vkCode != VK_F8 &&
                k->vkCode != VK_F9)
            {
                Action();
            }
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
    LPARAM lParam)
{
    if (code >= 0)
    {
        if (wParam == WM_LBUTTONDOWN ||
            wParam == WM_RBUTTONDOWN ||
            wParam == WM_MBUTTONDOWN ||
            wParam == WM_XBUTTONDOWN)
        {
            Action();
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
// WINDOW
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
        // Real-time update
        // ----------------------------------------------------

        case WM_TIMER:

            Advance();

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE
            );

            return 0;


        // ----------------------------------------------------
        // Mouse hit testing
        // ----------------------------------------------------

        case WM_NCHITTEST:

            if (!clickable)
            {
                return HTTRANSPARENT;
            }

            return DefWindowProcW(
                h,
                msg,
                wParam,
                lParam
            );


        // ----------------------------------------------------
        // Painting
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


            // Background

            HBRUSH background =
                CreateSolidBrush(
                    RGB(20, 20, 20)
                );

            FillRect(
                dc,
                &r,
                background
            );

            DeleteObject(
                background
            );


            SetBkMode(
                dc,
                TRANSPARENT
            );

            SetTextColor(
                dc,
                RGB(255, 255, 255)
            );


            // Font

            HFONT font =
                CreateFontW(
                    -22,
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
                    L"Segoe UI"
                );

            HFONT oldFont =
                reinterpret_cast<HFONT>(
                    SelectObject(
                        dc,
                        font
                    )
                );


            // APM ONLY

            std::wstring text =
                L"APM " +
                std::to_wstring(total);

            DrawTextW(
                dc,
                text.c_str(),
                -1,
                &r,
                DT_CENTER |
                DT_VCENTER |
                DT_SINGLELINE
            );


            SelectObject(
                dc,
                oldFont
            );

            DeleteObject(
                font
            );


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
                UnhookWindowsHookEx(
                    keyboardHook
                );

            if (mouseHook)
                UnhookWindowsHookEx(
                    mouseHook
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

    RegisterClassW(
        &wc
    );


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
        return 1;


    SetLayeredWindowAttributes(
        hwnd,
        0,
        225,
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

        return 1;
    }


    AddTrayIcon();


    // 100 ms = much more responsive display

    SetTimer(
        hwnd,
        1,
        100,
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

        DispatchMessageW(&msg);
    }


    return 0;
}
