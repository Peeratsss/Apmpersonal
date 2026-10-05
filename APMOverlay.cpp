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

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

static HWND hwnd = nullptr;
static HHOOK keyboardHook = nullptr;
static HHOOK mouseHook = nullptr;

static std::vector<ULONGLONG> actions;
static CRITICAL_SECTION actionLock;

static bool clickable = false;

static int overlayWidth = 150;
static int overlayHeight = 45;

static int overlayX = 20;
static int overlayY = 20;

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
// Save / Load window position and size
// ============================================================

std::wstring GetSaveFilePath()
{
    wchar_t exePath[MAX_PATH]{};

    GetModuleFileNameW(
        nullptr,
        exePath,
        MAX_PATH
    );

    std::wstring path(exePath);

    size_t slash =
        path.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
    {
        path.resize(slash + 1);
    }

    path += L"APMOverlay.txt";

    return path;
}

void SaveWindowSettings()
{
    if (!hwnd)
        return;

    RECT r{};

    if (!GetWindowRect(hwnd, &r))
        return;

    overlayX =
        r.left;

    overlayY =
        r.top;

    overlayWidth =
        r.right - r.left;

    overlayHeight =
        r.bottom - r.top;

    std::wofstream file(
        GetSaveFilePath()
    );

    if (!file.is_open())
        return;

    file << L"X=" << overlayX << L"\n";
    file << L"Y=" << overlayY << L"\n";
    file << L"Width=" << overlayWidth << L"\n";
    file << L"Height=" << overlayHeight << L"\n";
}

void LoadWindowSettings()
{
    std::wifstream file(
        GetSaveFilePath()
    );

    if (!file.is_open())
        return;

    std::wstring line;

    while (std::getline(file, line))
    {
        size_t equals =
            line.find(L'=');

        if (equals == std::wstring::npos)
            continue;

        std::wstring key =
            line.substr(0, equals);

        std::wstring value =
            line.substr(equals + 1);

        try
        {
            int number =
                std::stoi(value);

            if (key == L"X")
                overlayX = number;

            else if (key == L"Y")
                overlayY = number;

            else if (key == L"Width")
                overlayWidth = number;

            else if (key == L"Height")
                overlayHeight = number;
        }
        catch (...)
        {
            // Ignore invalid values
        }
    }

    // Safety limits
    if (overlayWidth < 50)
        overlayWidth = 50;

    if (overlayHeight < 20)
        overlayHeight = 20;

    if (overlayWidth > 2000)
        overlayWidth = 2000;

    if (overlayHeight > 1000)
        overlayHeight = 1000;
}


// ============================================================
// APM
// ============================================================

void RemoveOldActionsLocked()
{
    ULONGLONG now =
        GetTickCount64();

    const ULONGLONG window =
        60000;

    ULONGLONG cutoff =
        (now > window)
        ? now - window
        : 0;

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
    EnterCriticalSection(
        &actionLock
    );

    RemoveOldActionsLocked();

    LeaveCriticalSection(
        &actionLock
    );
}

void Action()
{
    EnterCriticalSection(
        &actionLock
    );

    actions.push_back(
        GetTickCount64()
    );

    RemoveOldActionsLocked();

    LeaveCriticalSection(
        &actionLock
    );
}

int GetAPM()
{
    EnterCriticalSection(
        &actionLock
    );

    RemoveOldActionsLocked();

    int result =
        static_cast<int>(
            actions.size()
        );

    LeaveCriticalSection(
        &actionLock
    );

    return result;
}

void ResetAPM()
{
    EnterCriticalSection(
        &actionLock
    );

    actions.clear();

    LeaveCriticalSection(
        &actionLock
    );

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE
    );
}


// ============================================================
// Keyboard
// ============================================================

LRESULT CALLBACK KeyboardProc(
    int code,
    WPARAM wParam,
    LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* k =
            reinterpret_cast<KBDLLHOOKSTRUCT*>(
                lParam
            );

        if (k != nullptr &&
            k->vkCode < 256)
        {
            UINT vk =
                k->vkCode;

            if (wParam == WM_KEYDOWN ||
                wParam == WM_SYSKEYDOWN)
            {
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
            else if (wParam == WM_KEYUP ||
                     wParam == WM_SYSKEYUP)
            {
                keyRepeatCount[vk] = 0;
            }
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
// Mouse
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
// Tray
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

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{};

    nid.cbSize =
        sizeof(nid);

    nid.hWnd =
        hwnd;

    nid.uID =
        ID_TRAY;

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

    nid.cbSize =
        sizeof(nid);

    nid.hWnd =
        hwnd;

    nid.uID =
        ID_TRAY;

    Shell_NotifyIconW(
        NIM_DELETE,
        &nid
    );
}


// ============================================================
// Clickable Mode
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
        exStyle &=
            ~WS_EX_TRANSPARENT;

        style |=
            WS_THICKFRAME;
    }
    else
    {
        exStyle |=
            WS_EX_TRANSPARENT;

        style &=
            ~WS_THICKFRAME;
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
        overlayX,
        overlayY,
        overlayWidth,
        overlayHeight,
        SWP_NOACTIVATE |
        SWP_FRAMECHANGED
    );

    InvalidateRect(
        hwnd,
        nullptr,
        TRUE
    );

    UpdateWindow(hwnd);
}

void ToggleClickable()
{
    SetClickable(!clickable);
}


// ============================================================
// Drawing
// ============================================================

void DrawOverlay()
{
    RECT wr{};

    GetWindowRect(
        hwnd,
        &wr
    );

    int width =
        wr.right - wr.left;

    int height =
        wr.bottom - wr.top;

    if (width < 1)
        width = 1;

    if (height < 1)
        height = 1;

    overlayX =
        wr.left;

    overlayY =
        wr.top;

    overlayWidth =
        width;

    overlayHeight =
        height;

    HDC screenDC =
        GetDC(nullptr);

    if (!screenDC)
        return;

    HDC memDC =
        CreateCompatibleDC(
            screenDC
        );

    if (!memDC)
    {
        ReleaseDC(
            nullptr,
            screenDC
        );

        return;
    }

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

    void* bits =
        nullptr;

    HBITMAP bitmap =
        CreateDIBSection(
            screenDC,
            &bmi,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0
        );

    if (!bitmap)
    {
        DeleteDC(memDC);

        ReleaseDC(
            nullptr,
            screenDC
        );

        return;
    }

    HBITMAP oldBitmap =
        static_cast<HBITMAP>(
            SelectObject(
                memDC,
                bitmap
            )
        );

    const size_t byteCount =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        4;

    ZeroMemory(
        bits,
        byteCount
    );


    // --------------------------------------------------------
    // Font
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
            L"Arial"
        );

    HFONT oldFont =
        static_cast<HFONT>(
            SelectObject(
                memDC,
                font
            )
        );

    SetBkMode(
        memDC,
        TRANSPARENT
    );

    SetTextColor(
        memDC,
        RGB(255, 255, 255)
    );


    // --------------------------------------------------------
    // Top resize bar
    // --------------------------------------------------------

    if (clickable)
    {
        RECT bar{};

        bar.left = 0;
        bar.top = 0;
        bar.right = width;
        bar.bottom = 6;

        HBRUSH brush =
            CreateSolidBrush(
                RGB(255, 255, 255)
            );

        FillRect(
            memDC,
            &bar,
            brush
        );

        DeleteObject(brush);
    }


    // --------------------------------------------------------
    // APM text
    // --------------------------------------------------------

    std::wstring text =
        L"APM " +
        std::to_wstring(
            GetAPM()
        );

    RECT textRect{};

    textRect.left = 0;

    textRect.top =
        clickable ? 4 : 0;

    textRect.right =
        width;

    textRect.bottom =
        height;

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


    // --------------------------------------------------------
    // Convert white pixels to alpha
    // --------------------------------------------------------

    SelectObject(
        memDC,
        oldFont
    );

    DeleteObject(font);

    DWORD* pixels =
        static_cast<DWORD*>(bits);

    const size_t pixelCount =
        static_cast<size_t>(width) *
        static_cast<size_t>(height);

    for (size_t i = 0;
         i < pixelCount;
         ++i)
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
                (
                    static_cast<int>(red) +
                    static_cast<int>(green) +
                    static_cast<int>(blue)
                ) / 3
            );

        if (brightness == 0)
        {
            pixels[i] =
                0x00000000;
        }
        else
        {
            pixels[i] =
                (static_cast<DWORD>(
                    brightness
                ) << 24)
                |
                0x00FFFFFF;
        }
    }


    // --------------------------------------------------------
    // Display
    // --------------------------------------------------------

    POINT source{
        0,
        0
    };

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

    blend.BlendFlags =
        0;

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

    ReleaseDC(
        nullptr,
        screenDC
    );
}


// ============================================================
// Window procedure
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
        {
            if (lParam ==
                WM_RBUTTONUP)
            {
                ShowTrayMenu();
            }

            return 0;
        }


        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case ID_CLICKABLE:
                    ToggleClickable();
                    break;

                case ID_RESET:
                    ResetAPM();
                    break;

                case ID_EXIT:
                    SaveWindowSettings();
                    DestroyWindow(hwnd);
                    break;
            }

            return 0;
        }


        case WM_TIMER:
        {
            RemoveOldActions();

            DrawOverlay();

            return 0;
        }


        case WM_NCHITTEST:
        {
            if (!clickable)
                return HTTRANSPARENT;

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

            int height =
                r.bottom - r.top;

            const int grip = 8;


            // Top resize
            if (y < grip)
            {
                if (x < grip)
                    return HTTOPLEFT;

                if (x >= width - grip)
                    return HTTOPRIGHT;

                return HTTOP;
            }


            // Bottom resize
            if (y >= height - grip)
            {
                if (x < grip)
                    return HTBOTTOMLEFT;

                if (x >= width - grip)
                    return HTBOTTOMRIGHT;

                return HTBOTTOM;
            }


            // Left/right resize
            if (x < grip)
                return HTLEFT;

            if (x >= width - grip)
                return HTRIGHT;


            // Body moves window
            return HTCAPTION;
        }


        case WM_MOVE:
        {
            RECT r{};

            if (GetWindowRect(
                    hwnd,
                    &r))
            {
                overlayX =
                    r.left;

                overlayY =
                    r.top;

                SaveWindowSettings();
            }

            return 0;
        }


        case WM_SIZE:
        {
            int newWidth =
                LOWORD(lParam);

            int newHeight =
                HIWORD(lParam);

            if (newWidth < 50)
                newWidth = 50;

            if (newHeight < 20)
                newHeight = 20;

            overlayWidth =
                newWidth;

            overlayHeight =
                newHeight;

            SaveWindowSettings();

            DrawOverlay();

            return 0;
        }


        case WM_EXITSIZEMOVE:
        {
            SaveWindowSettings();

            return 0;
        }


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
        {
            SaveWindowSettings();

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
    }

    return DefWindowProcW(
        h,
        msg,
        wParam,
        lParam
    );
}


// ============================================================
// WinMain
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


    // Load previous position/size
    LoadWindowSettings();


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

    RegisterClassW(&wc);


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

            overlayX,
            overlayY,

            overlayWidth,
            overlayHeight,

            nullptr,
            nullptr,

            instance,
            nullptr
        );

    if (!hwnd)
    {
        DeleteCriticalSection(
            &actionLock
        );

        return 1;
    }


    // Initial rendering
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

        return 1;
    }


    // --------------------------------------------------------
    // Tray
    // --------------------------------------------------------

    AddTrayIcon();


    // --------------------------------------------------------
    // Refresh every 5 seconds
    // --------------------------------------------------------

    SetTimer(
        hwnd,
        1,
        5000,
        nullptr
    );


    // --------------------------------------------------------
    // Show
    // --------------------------------------------------------

    ShowWindow(
        hwnd,
        SW_SHOWNOACTIVATE
    );


    // --------------------------------------------------------
    // Message loop
    // --------------------------------------------------------

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
