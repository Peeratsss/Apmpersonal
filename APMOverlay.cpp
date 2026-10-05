#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <array>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

static HWND hwnd;
static HHOOK keyboardHook;
static HHOOK mouseHook;

static std::array<int, 60> buckets{};
static int bucket = 0;
static long long total = 0;
static ULONGLONG lastSecond = 0;
static bool clickable = false;

enum {
    ID_TRAY = 100,
    ID_CLICKABLE = 101,
    ID_RESET = 102,
    ID_EXIT = 103
};

void Advance()
{
    ULONGLONG now = GetTickCount64() / 1000;

    if (!lastSecond) {
        lastSecond = now;
        return;
    }

    ULONGLONG diff = now - lastSecond;

    if (!diff) return;

    if (diff >= 60) {
        buckets.fill(0);
        total = 0;
        bucket = 0;
    } else {
        for (ULONGLONG i = 0; i < diff; ++i) {
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
    InvalidateRect(hwnd, nullptr, FALSE);
}

void SetClickable(bool value)
{
    clickable = value;

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    if (clickable)
        style &= ~WS_EX_TRANSPARENT;
    else
        style |= WS_EX_TRANSPARENT;

    SetWindowLongPtr(hwnd, GWL_EXSTYLE, style);

    SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOACTIVATE |
        SWP_FRAMECHANGED
    );

    InvalidateRect(hwnd, nullptr, TRUE);
}

void ToggleClickable()
{
    SetClickable(!clickable);
}

LRESULT CALLBACK KeyboardProc(
    int code,
    WPARAM wParam,
    LPARAM lParam)
{
    if (code >= 0 &&
        (wParam == WM_KEYDOWN ||
         wParam == WM_SYSKEYDOWN))
    {
        KBDLLHOOKSTRUCT* k =
            (KBDLLHOOKSTRUCT*)lParam;

        // Don't count our own hotkeys.
        if (k->vkCode != VK_F8 &&
            k->vkCode != VK_F9)
        {
            Action();
        }
    }

    return CallNextHookEx(
        keyboardHook,
        code,
        wParam,
        lParam
    );
}

LRESULT CALLBACK MouseProc(
    int code,
    WPARAM wParam,
    LPARAM lParam)
{
    if (code >= 0) {
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

void ShowTrayMenu()
{
    POINT p;
    GetCursorPos(&p);

    HMENU menu = CreatePopupMenu();

    AppendMenu(
        menu,
        MF_STRING | (clickable ? MF_CHECKED : 0),
        ID_CLICKABLE,
        L"Clickable Mode"
    );

    AppendMenu(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    AppendMenu(
        menu,
        MF_STRING,
        ID_RESET,
        L"Reset APM"
    );

    AppendMenu(
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

void AddTrayIcon()
{
    NOTIFYICONDATA nid{};

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags =
        NIF_ICON |
        NIF_MESSAGE |
        NIF_TIP;

    nid.uCallbackMessage =
        WM_USER + 1;

    nid.hIcon =
        LoadIcon(nullptr, IDI_APPLICATION);

    lstrcpyW(
        nid.szTip,
        L"APM Overlay"
    );

    Shell_NotifyIcon(
        NIM_ADD,
        &nid
    );
}

void RemoveTrayIcon()
{
    NOTIFYICONDATA nid{};

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY;

    Shell_NotifyIcon(
        NIM_DELETE,
        &nid
    );
}

LRESULT CALLBACK WindowProc(
    HWND h,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
    case WM_USER + 1:

        if (lParam == WM_RBUTTONUP)
            ShowTrayMenu();

        return 0;

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

    case WM_TIMER:

        Advance();
        InvalidateRect(
            hwnd,
            nullptr,
            FALSE
        );

        return 0;

    case WM_LBUTTONDOWN:

        if (clickable) {
            ReleaseCapture();

            SendMessage(
                hwnd,
                WM_NCLBUTTONDOWN,
                HTCAPTION,
                0
            );
        }

        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        HDC dc =
            BeginPaint(
                hwnd,
                &ps
            );

        RECT r;
        GetClientRect(hwnd, &r);

        HBRUSH background =
            CreateSolidBrush(
                RGB(20,20,20)
            );

        FillRect(
            dc,
            &r,
            background
        );

        DeleteObject(background);

        SetBkMode(
            dc,
            TRANSPARENT
        );

        SetTextColor(
            dc,
            RGB(255,255,255)
        );

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

        HFONT old =
            (HFONT)SelectObject(
                dc,
                font
            );

        std::wstring text =
            L"APM  " +
            std::to_wstring(total);

        RECT textRect = r;
        textRect.left += 10;

        DrawTextW(
            dc,
            text.c_str(),
            -1,
            &textRect,
            DT_LEFT |
            DT_VCENTER |
            DT_SINGLELINE
        );

        RECT mode = r;
        mode.left = r.right - 90;
        mode.right = r.right - 8;
        mode.top = 8;
        mode.bottom = r.bottom - 8;

        HBRUSH modeBrush =
            CreateSolidBrush(
                clickable
                ? RGB(70,120,70)
                : RGB(55,55,55)
            );

        FillRect(
            dc,
            &mode,
            modeBrush
        );

        DeleteObject(modeBrush);

        std::wstring status =
            clickable
            ? L"CLICK"
            : L"PASS";

        DrawTextW(
            dc,
            status.c_str(),
            -1,
            &mode,
            DT_CENTER |
            DT_VCENTER |
            DT_SINGLELINE
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

    return DefWindowProc(
        h,
        msg,
        wParam,
        lParam
    );
}

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
        LoadCursor(
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
            190,
            48,

            nullptr,
            nullptr,
            instance,
            nullptr
        );

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

    AddTrayIcon();

    SetTimer(
        hwnd,
        1,
        250,
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
        DispatchMessage(&msg);
    }

    return 0;
}
