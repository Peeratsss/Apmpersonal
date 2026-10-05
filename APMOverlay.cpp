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

// Current overlay dimensions.
// These stay the same when switching between modes.
static int overlayWidth = 150;
static int overlayHeight = 45;

static const int MAX_KEY_REPEATS = 5;
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
// APM
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


void Action()
{
    EnterCriticalSection(&actionLock);

    actions.push_back(GetTickCount64());

    RemoveOldActionsLocked();

    LeaveCriticalSection(&actionLock);
}


int GetAPM()
{
    EnterCriticalSection(&actionLock);

    RemoveOldActionsLocked();

    int result =
        static_cast<int>(actions.size());

    LeaveCriticalSection(&actionLock);

    return result;
}


void ResetAPM()
{
    EnterCriticalSection(&actionLock);

    actions.clear();

    LeaveCriticalSection(&actionLock);

    InvalidateRect(hwnd, nullptr, FALSE);
}


// ============================================================
// KEYBOARD HOOK
//
// Every key can repeat up to 5 times while held.
// Release = reset that key's repeat counter.
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

        if (k && k->vkCode < 256)
        {
            UINT vk = k->vkCode;

            if (wParam == WM_KEYDOWN ||
                wParam == WM_SYSKEYDOWN)
            {
                if (vk != VK_F8 &&
                    vk != VK_F9)
                {
                    if (keyRepeatCount[vk] < MAX_KEY_REPEATS)
                    {
                        keyRepeatCount[vk]++;
                        Action();
                    }
                }
            }
            else if (wParam == WM_KEYUP ||
                     wParam == WM_SYSKEYUP)
            {
                keyRepeatCount[vk] = 0;
            }
        }
    }

    // Never block keyboard input.
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

    return CallNextHookEx(
        nullptr,
        code,
        wParam,
        lParam
    );
}


// ============================================================
// TRAY
// ============================================================

void ShowTrayMenu()
{
    POINT p{};
    GetCursorPos(&p);

    HMENU menu = CreatePopupMenu();

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
        // Allow interaction.
        exStyle &= ~WS_EX_TRANSPARENT;

        // Enable resizing.
        style |= WS_THICKFRAME;
    }
    else
    {
        // Click-through.
        exStyle |= WS_EX_TRANSPARENT;

        // No visible Windows frame.
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

    // IMPORTANT:
    // Keep exactly the same physical window size.
    SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        overlayWidth,
        overlayHeight,
        SWP_NOMOVE |
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
// DRAW TRANSPARENT OVERLAY
// ============================================================

void DrawOverlay()
{
    RECT wr{};
    GetWindowRect(hwnd, &wr);

    int width =
        wr.right - wr.left;

    int height =
        wr.bottom - wr.top;

    if (width < 1)
        width = 1;

    if (height < 1)
        height = 1;

    overlayWidth = width;
    overlayHeight = height;


    HDC screenDC =
        GetDC(nullptr);

    HDC memDC =
        CreateCompatibleDC(screenDC);


    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);

    bmi.bmiHeader.biWidth =
        width;

    bmi.bmiHeader.biHeight =
        -height;

    bmi.bmiHeader.biPlanes =
        1;

    bmi.bmiHeader.biBitCount =
        32;

    bmi.bmiHeader.biCompression =
        BI_RGB;


    void* bits = nullptr;

    HBITMAP bitmap =
        CreateDIBSection(
            screenDC,
            &bmi,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0
        );

    HBITMAP oldBitmap =
        (HBITMAP)SelectObject(
            memDC,
            bitmap
        );


    // Completely transparent background.
    ZeroMemory(
        bits,
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        4
    );


    // --------------------------------------------------------
    // Font scales with the actual overlay height.
    // Same calculation in BOTH modes.
    // --------------------------------------------------------

    int fontHeight =
        (height * 62) / 100;

    if (fontHeight < 8)
        fontHeight = 8;

    if (fontHeight > 300)
        fontHeight = 300;


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
            DEFAULT_PITCH,
            L"Radiance Sans"
        );


    HFONT oldFont =
        (HFONT)SelectObject(
            memDC,
            font
        );


    SetBkMode(
        memDC,
        TRANSPARENT
    );

    SetTextColor(
        memDC,
        RGB(255, 255, 255)
    );


    std::wstring text =
        L"APM " +
        std::to_wstring(
            GetAPM()
        );


    RECT textRect{
        0,
        0,
        width,
        height
    };


    DrawTextW(
        memDC,
        text.c_str(),
        -1,
        &textRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE |
        DT_NOPREFIX
    );


    SelectObject(
        memDC,
        oldFont
    );

    DeleteObject(font);


    // --------------------------------------------------------
    // Make the pixels containing the text opaque.
    // Transparent background remains transparent.
    // --------------------------------------------------------

    DWORD* pixels =
        static_cast<DWORD*>(bits);

    const size_t pixelCount =
        static_cast<size_t>(width) *
        static_cast<size_t>(height);

    for (size_t i = 0; i < pixelCount; ++i)
    {
        BYTE blue =
            static_cast<BYTE>(
                pixels[i] & 0xFF
            );

        BYTE green =
            static_cast<BYTE>(
                (pixels[i] >> 8) & 0xFF
            );

        BYTE red =
            static_cast<BYTE>(
                (pixels[i] >> 16) & 0xFF
            );

        BYTE brightness =
            static_cast<BYTE>(
                (red + green + blue) / 3
            );

        if (brightness > 0)
        {
            pixels[i] =
                (static_cast<DWORD>(brightness) << 24) |
                0x00FFFFFF;
        }
        else
        {
            pixels[i] = 0;
        }
    }


    POINT source{0, 0};

    POINT position{
        wr.left,
        wr.top
    };

    SIZE size{
        width,
        height
    };


    BLENDFUNCTION blend{};
    blend.BlendOp =
        AC_SRC_OVER;

    blend.SourceConstantAlpha =
        255;

    blend.AlphaFormat =
        AC_SRC_ALPHA;


    UpdateLayeredWindow(
        hwnd,
        screenDC,
        &position,
        &size,
        memDC,
        &source,
        0,
        &blend,
        ULW_ALPHA
    );


    SelectObject(
        memDC,
        oldBitmap
    );

    DeleteObject(bitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
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
        case WM_TRAYICON:

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

            RemoveOldActions();

            DrawOverlay();

            return 0;


        // ----------------------------------------------------
        // Click-through in pass mode.
        // ----------------------------------------------------

        case WM_NCHITTEST:

            if (!clickable)
                return HTTRANSPARENT;

            {
                POINT p{
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam)
                };

                RECT r{};
                GetWindowRect(
                    hwnd,
                    &r
                );

                int x =
                    p.x - r.left;

                int y =
                    p.y - r.top;

                int width =
                    r.right - r.left;

                const int grip = 10;

                // Upper bar:
                // corners resize diagonally,
                // center resizes vertically.

                if (y < grip)
                {
                    if (x < grip)
                        return HTTOPLEFT;

                    if (x >= width - grip)
                        return HTTOPRIGHT;

                    return HTTOP;
                }

                // Whole body relocates.
                return HTCAPTION;
            }


        case WM_SIZE:

            overlayWidth =
                LOWORD(lParam);

            overlayHeight =
                HIWORD(lParam);

            if (overlayWidth < 50)
                overlayWidth = 50;

            if (overlayHeight < 20)
                overlayHeight = 20;

            DrawOverlay();

            return 0;


        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            BeginPaint(
                hwnd,
                &ps
            );

            EndPaint(
                hwnd,
                &ps
            );

            DrawOverlay();

            return 0;
        }


        case WM_ERASEBKGND:

            return 1;


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
    // Load Radiance Sans
    // --------------------------------------------------------

    wchar_t exePath[MAX_PATH]{};

    GetModuleFileNameW(
        nullptr,
        exePath,
        MAX_PATH
    );

    std::wstring fontPath(
        exePath
    );

    size_t slash =
        fontPath.find_last_of(
            L"\\/"
        );

    if (slash != std::wstring::npos)
    {
        fontPath.resize(
            slash + 1
        );
    }

    fontPath +=
        L"Radiance_Sans.ttf";


    AddFontResourceExW(
        fontPath.c_str(),
        FR_PRIVATE,
        nullptr
    );


    // --------------------------------------------------------
    // Window class
    // --------------------------------------------------------

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


    // --------------------------------------------------------
    // Create overlay
    // --------------------------------------------------------

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
            overlayWidth,
            overlayHeight,

            nullptr,
            nullptr,
            instance,
            nullptr
        );


    if (!hwnd)
    {
        RemoveFontResourceExW(
            fontPath.c_str(),
            FR_PRIVATE,
            nullptr
        );

        DeleteCriticalSection(
            &actionLock
        );

        return 1;
    }


    // Initial transparent rendering.
    DrawOverlay();


    // --------------------------------------------------------
    // Hooks
    // --------------------------------------------------------

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
            fontPath.c_str(),
            FR_PRIVATE,
            nullptr
        );

        DeleteCriticalSection(
            &actionLock
        );

        return 1;
    }


    AddTrayIcon();


    // --------------------------------------------------------
    // Update every 5 seconds
    // --------------------------------------------------------

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
        TranslateMessage(
            &msg
        );

        DispatchMessageW(
            &msg
        );
    }


    RemoveFontResourceExW(
        fontPath.c_str(),
        FR_PRIVATE,
        nullptr
    );

    return 0;
}
