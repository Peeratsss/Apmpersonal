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

// --------------------------------------------------------
// Save / Load window settings
// --------------------------------------------------------

std::wstring GetSaveFilePath()
{
    wchar_t exePath[MAX_PATH]{};

    GetModuleFileNameW(
        nullptr,
        exePath,
        MAX_PATH
    );

    std::wstring path(exePath);

    size_t slash = path.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        path.resize(slash + 1);

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

    overlayX = r.left;
    overlayY = r.top;

    overlayWidth = r.right - r.left;
    overlayHeight = r.bottom - r.top;

    std::wofstream file(GetSaveFilePath());

    if (!file.is_open())
        return;

    file << L"X=" << overlayX << L"\n";
    file << L"Y=" << overlayY << L"\n";
    file << L"Width=" << overlayWidth << L"\n";
    file << L"Height=" << overlayHeight << L"\n";
}

void LoadWindowSettings()
{
    std::wifstream file(GetSaveFilePath());

    if (!file.is_open())
        return;

    std::wstring line;

    while (std::getline(file, line))
    {
        size_t equals = line.find(L'=');

        if (equals == std::wstring::npos)
            continue;

        std::wstring key =
            line.substr(0, equals);

        std::wstring value =
            line.substr(equals + 1);

        try
        {
            int number = std::stoi(value);

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
        }
    }

    if (overlayWidth < 50)
        overlayWidth = 50;

    if (overlayHeight < 20)
        overlayHeight = 20;

    if (overlayWidth > 2000)
        overlayWidth = 2000;

    if (overlayHeight > 1000)
        overlayHeight = 1000;
}

// --------------------------------------------------------
// APM tracking
// --------------------------------------------------------

void RemoveOldActionsLocked()
{
    ULONGLONG now = GetTickCount64();

    const ULONGLONG window = 60000;

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
    EnterCriticalSection(&actionLock);

    RemoveOldActionsLocked();

    LeaveCriticalSection(&actionLock);
}

void Action()
{
    EnterCriticalSection(&actionLock);

    actions.push_back(
        GetTickCount64()
    );

    RemoveOldActionsLocked();

    LeaveCriticalSection(&actionLock);
}

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

// --------------------------------------------------------
// Keyboard hook
// --------------------------------------------------------

LRESULT CALLBACK KeyboardProc(
    int code,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (code == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* k =
            reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (k != nullptr && k->vkCode < 256)
        {
            UINT vk = k->vkCode;

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

// --------------------------------------------------------
// Mouse hook
// --------------------------------------------------------

LRESULT CALLBACK MouseProc(
    int code,
    WPARAM wParam,
    LPARAM lParam
)
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

// --------------------------------------------------------
// Tray icon
// --------------------------------------------------------

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{};

    nid.cbSize =
        sizeof(NOTIFYICONDATAW);

    nid.hWnd = hwnd;

    nid.uID = ID_TRAY;

    nid.uFlags =
        NIF_MESSAGE |
        NIF_ICON |
        NIF_TIP;

    nid.uCallbackMessage =
        WM_TRAYICON;

    nid.hIcon =
        LoadIcon(
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
        sizeof(NOTIFYICONDATAW);

    nid.hWnd = hwnd;

    nid.uID = ID_TRAY;

    Shell_NotifyIconW(
        NIM_DELETE,
        &nid
    );
}

void ShowTrayMenu()
{
    POINT pt{};

    GetCursorPos(&pt);

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
        MF_STRING,
        ID_RESET,
        L"Reset APM"
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
        ID_EXIT,
        L"Exit"
    );

    SetForegroundWindow(hwnd);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        pt.x,
        pt.y,
        0,
        hwnd,
        nullptr
    );

    DestroyMenu(menu);
}

// --------------------------------------------------------
// Clickable / pass-through mode
// --------------------------------------------------------

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
        exStyle &= ~WS_EX_TRANSPARENT;

        style |= WS_THICKFRAME;
    }
    else
    {
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

// --------------------------------------------------------
// Draw overlay
// --------------------------------------------------------

void DrawOverlay()
{
    if (!hwnd)
        return;

    RECT client{};

    GetClientRect(
        hwnd,
        &client
    );

    int width =
        client.right - client.left;

    int height =
        client.bottom - client.top;

    if (width <= 0 || height <= 0)
        return;

    HDC screenDC =
        GetDC(nullptr);

    HDC memoryDC =
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
            memoryDC,
            bitmap
        );

    // Completely transparent background.
    memset(
        bits,
        0,
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        4
    );

    // ----------------------------------------------------
    // Dota-style font
    // ----------------------------------------------------

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
            FW_HEAVY,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY,
            DEFAULT_PITCH,
            L"Impact"
        );

    HFONT oldFont =
        (HFONT)SelectObject(
            memoryDC,
            font
        );

    SetBkMode(
        memoryDC,
        TRANSPARENT
    );

    SetTextColor(
        memoryDC,
        RGB(255, 255, 255)
    );

    std::wstring text =
        L"APM " +
        std::to_wstring(
            GetAPM()
        );

    RECT textRect{};

    textRect.left = 0;

    textRect.top =
        clickable ? 4 : 0;

    textRect.right = width;

    textRect.bottom = height;

    DrawTextW(
        memoryDC,
        text.c_str(),
        -1,
        &textRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE |
        DT_NOPREFIX
    );

    // ----------------------------------------------------
    // Clickable mode top resize bar
    // ----------------------------------------------------

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
            memoryDC,
            &bar,
            brush
        );

        DeleteObject(brush);
    }

    SelectObject(
        memoryDC,
        oldFont
    );

    DeleteObject(font);

    SelectObject(
        memoryDC,
        oldBitmap
    );

    // ----------------------------------------------------
    // Convert brightness to alpha
    // ----------------------------------------------------

    DWORD* pixels =
        static_cast<DWORD*>(bits);

    size_t pixelCount =
        static_cast<size_t>(width) *
        static_cast<size_t>(height);

    for (size_t i = 0;
         i < pixelCount;
         ++i)
    {
        BYTE r =
            (BYTE)((pixels[i] >> 16) & 0xFF);

        BYTE g =
            (BYTE)((pixels[i] >> 8) & 0xFF);

        BYTE b =
            (BYTE)(pixels[i] & 0xFF);

        BYTE brightness =
            (BYTE)((r + g + b) / 3);

        pixels[i] =
            (DWORD(brightness) << 24) |
            (DWORD(255) << 16) |
            (DWORD(255) << 8) |
            DWORD(255);
    }

    POINT position{};

    position.x = overlayX;
    position.y = overlayY;

    SIZE size{};

    size.cx = width;
    size.cy = height;

    POINT source{};

    source.x = 0;
    source.y = 0;

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
        memoryDC,
        &source,
        0,
        &blend,
        ULW_ALPHA
    );

    DeleteObject(bitmap);

    DeleteDC(memoryDC);

    ReleaseDC(
        nullptr,
        screenDC
    );
}

// --------------------------------------------------------
// Window procedure
// --------------------------------------------------------

LRESULT CALLBACK WndProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (message)
    {
        case WM_TRAYICON:
        {
            if (lParam == WM_RBUTTONUP ||
                lParam == WM_CONTEXTMENU)
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
                    DestroyWindow(hwnd);
                    break;
            }

            return 0;
        }

        case WM_TIMER:
        {
            if (wParam == 1)
            {
                RemoveOldActions();

                DrawOverlay();
            }

            return 0;
        }

        case WM_NCHITTEST:
        {
            if (!clickable)
            {
                return HTTRANSPARENT;
            }

            POINT pt{};

            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);

            ScreenToClient(
                window,
                &pt
            );

            RECT r{};

            GetClientRect(
                window,
                &r
            );

            const int grip = 8;

            bool left =
                pt.x < grip;

            bool right =
                pt.x >= r.right - grip;

            bool top =
                pt.y < grip;

            bool bottom =
                pt.y >= r.bottom - grip;

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

        case WM_MOVE:
        case WM_SIZE:
        {
            if (clickable)
            {
                RECT r{};

                if (GetWindowRect(
                        window,
                        &r))
                {
                    overlayX = r.left;
                    overlayY = r.top;

                    overlayWidth =
                        r.right - r.left;

                    overlayHeight =
                        r.bottom - r.top;
                }
            }

            return 0;
        }

        case WM_EXITSIZEMOVE:
        {
            SaveWindowSettings();

            DrawOverlay();

            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            BeginPaint(
                window,
                &ps
            );

            EndPaint(
                window,
                &ps
            );

            return 0;
        }

        case WM_DESTROY:
        {
            KillTimer(
                window,
                1
            );

            SaveWindowSettings();

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
        window,
        message,
        wParam,
        lParam
    );
}

// --------------------------------------------------------
// WinMain
// --------------------------------------------------------

int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    LPSTR,
    int
)
{
    InitializeCriticalSection(
        &actionLock
    );

    LoadWindowSettings();

    const wchar_t CLASS_NAME[] =
        L"APMOverlayWindow";

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WndProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        CLASS_NAME;

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

            CLASS_NAME,
            L"APM Overlay",

            WS_POPUP,

            overlayX,
            overlayY,
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
            &actionLock
        );

        return 1;
    }

    DrawOverlay();

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

    AddTrayIcon();

    // ----------------------------------------------------
    // Refresh every 1 second
    // ----------------------------------------------------

    SetTimer(
        hwnd,
        1,
        1000,
        nullptr
    );

    ShowWindow(
        hwnd,
        SW_SHOWNOACTIVATE
    );

    UpdateWindow(hwnd);

    MSG msg{};

    while (GetMessageW(
        &msg,
        nullptr,
        0,
        0))
    {
        TranslateMessage(&msg);

        DispatchMessageW(&msg);
    }

    return 0;
}
