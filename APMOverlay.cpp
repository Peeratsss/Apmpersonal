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

static bool clickableMode = false;

static int overlayX = 20;
static int overlayY = 20;
static int overlayWidth = 150;
static int overlayHeight = 45;

static const int TOP_BAR_HEIGHT = 6;
static const int RENDER_SCALE = 2;

static CRITICAL_SECTION actionLock;

static std::vector<ULONGLONG> actions;
static ULONGLONG lastActionTime = 0;

static BYTE keyRepeatCount[256] = { 0 };

struct InputStat
{
    std::wstring name;
    unsigned long long count;
};

static std::vector<InputStat> inputStats;
static std::vector<std::wstring> inputTimeline;


// ============================================================
// TRAY
// ============================================================

#define WM_TRAYICON      (WM_USER + 1)

#define ID_TRAY_CLICK    1001
#define ID_TRAY_RESET    1002
#define ID_TRAY_INPUT    1003
#define ID_TRAY_EXIT     1004


// ============================================================
// FILE PATHS
// ============================================================

std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH] = {};

    GetModuleFileNameW(
        nullptr,
        path,
        MAX_PATH
    );

    std::wstring result(path);

    size_t slash =
        result.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        result.resize(slash);

    return result;
}


std::wstring GetSettingsPath()
{
    return GetExeDirectory() +
           L"\\APMOverlay.txt";
}


std::wstring GetInputStatsPath()
{
    return GetExeDirectory() +
           L"\\Inputs.txt";
}


// ============================================================
// UTF-8
// ============================================================

std::string WideToUTF8(
    const std::wstring& text
)
{
    if (text.empty())
        return "";

    int size =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.c_str(),
            -1,
            nullptr,
            0,
            nullptr,
            nullptr
        );

    if (size <= 0)
        return "";

    std::string result(
        size - 1,
        '\0'
    );

    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        &result[0],
        size,
        nullptr,
        nullptr
    );

    return result;
}


// ============================================================
// SETTINGS
// ============================================================

void SaveSettings()
{
    std::wofstream file(
        GetSettingsPath()
    );

    if (!file.is_open())
        return;

    file << L"X=" << overlayX << L"\n";
    file << L"Y=" << overlayY << L"\n";
    file << L"Width=" << overlayWidth << L"\n";
    file << L"Height=" << overlayHeight << L"\n";
}


void LoadSettings()
{
    std::wifstream file(
        GetSettingsPath()
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
            line.substr(
                0,
                equals
            );

        std::wstring value =
            line.substr(
                equals + 1
            );

        int number =
            _wtoi(
                value.c_str()
            );

        if (key == L"X")
            overlayX = number;

        else if (key == L"Y")
            overlayY = number;

        else if (key == L"Width")
            overlayWidth = number;

        else if (key == L"Height")
            overlayHeight = number;
    }

    if (overlayWidth < 50)
        overlayWidth = 50;

    if (overlayHeight < 20)
        overlayHeight = 20;
}


// ============================================================
// WINDOWS LOCAL CLOCK
// ============================================================

std::wstring GetCurrentTimeString()
{
    SYSTEMTIME time;

    // Reads the computer's Windows local clock.
    GetLocalTime(
        &time
    );

    int hour =
        time.wHour;

    bool pm =
        hour >= 12;

    int displayHour =
        hour % 12;

    if (displayHour == 0)
        displayHour = 12;

    wchar_t buffer[64] = {};

    swprintf_s(
        buffer,
        L"%02d:%02d:%02d.%03d %s",
        displayHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds,
        pm ? L"PM" : L"AM"
    );

    return buffer;
}


// ============================================================
// INPUT TRACKER FILE
// ============================================================

void SaveInputStats()
{
    std::ofstream file(
        GetInputStatsPath(),
        std::ios::out |
        std::ios::trunc |
        std::ios::binary
    );

    if (!file.is_open())
        return;

    file << "=== BUTTON PRESS COUNTS ===\n\n";

    for (const auto& stat : inputStats)
    {
        file
            << WideToUTF8(stat.name)
            << " = "
            << stat.count
            << "\n";
    }

    file << "\n\n";

    file << "=== INPUT TIMELINE ===\n\n";

    for (const auto& input : inputTimeline)
    {
        file
            << WideToUTF8(input)
            << "\n";
    }
}


// ============================================================
// RECORD BUTTON PRESS
// ============================================================

void RecordButtonPressLocked(
    const std::wstring& name
)
{
    bool found = false;

    for (auto& stat : inputStats)
    {
        if (stat.name == name)
        {
            stat.count++;
            found = true;
            break;
        }
    }

    if (!found)
    {
        InputStat stat;

        stat.name = name;
        stat.count = 1;

        inputStats.push_back(
            stat
        );
    }
}


// ============================================================
// RECORD TIMELINE EVENT
// ============================================================

void RecordTimelineLocked(
    const std::wstring& name
)
{
    std::wstring entry =
        GetCurrentTimeString();

    entry += L"\t";

    entry += name;

    inputTimeline.push_back(
        entry
    );
}


// ============================================================
// RECORD PRESS
// ============================================================

void RecordInputLocked(
    const std::wstring& name
)
{
    // Button count only counts presses.
    RecordButtonPressLocked(
        name
    );

    // Timeline records the exact Windows
    // local clock time including milliseconds.
    RecordTimelineLocked(
        name
    );

    SaveInputStats();
}


// ============================================================
// RECORD RELEASE
// ============================================================

void RecordReleaseLocked(
    const std::wstring& name
)
{
    RecordTimelineLocked(
        name + L"_UP"
    );

    SaveInputStats();
}


// ============================================================
// RESET APM ONLY
// ============================================================

void ResetAPM()
{
    EnterCriticalSection(
        &actionLock
    );

    actions.clear();

    lastActionTime = 0;

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
// RESET INPUT TRACKER ONLY
// ============================================================

void ResetInputTracker()
{
    EnterCriticalSection(
        &actionLock
    );

    inputStats.clear();

    inputTimeline.clear();

    for (int i = 0; i < 256; ++i)
        keyRepeatCount[i] = 0;

    SaveInputStats();

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
// REMOVE OLD APM ACTIONS
// ============================================================

void RemoveOldActionsLocked()
{
    const ULONGLONG now =
        GetTickCount64();

    const ULONGLONG window =
        60000ULL;

    if (
        lastActionTime != 0 &&
        now >= lastActionTime &&
        now - lastActionTime >= window
    )
    {
        actions.clear();

        lastActionTime = 0;

        return;
    }

    if (!actions.empty())
    {
        ULONGLONG cutoff =
            (now >= window)
            ? now - window
            : 0;

        auto it =
            std::upper_bound(
                actions.begin(),
                actions.end(),
                cutoff
            );

        actions.erase(
            actions.begin(),
            it
        );
    }

    if (actions.empty())
        lastActionTime = 0;
}


// ============================================================
// RECORD ACTION
// ============================================================

void Action(
    const std::wstring& inputName
)
{
    ULONGLONG now =
        GetTickCount64();

    EnterCriticalSection(
        &actionLock
    );

    RemoveOldActionsLocked();

    actions.push_back(
        now
    );

    lastActionTime = now;

    RecordInputLocked(
        inputName
    );

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
// GET APM
// ============================================================

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


// ============================================================
// KEY NAME
// ============================================================

std::wstring GetKeyName(
    DWORD vk
)
{
    switch (vk)
    {
    case VK_SPACE:
        return L"Space";

    case VK_RETURN:
        return L"Enter";

    case VK_ESCAPE:
        return L"Esc";

    case VK_TAB:
        return L"Tab";

    case VK_BACK:
        return L"Backspace";

    case VK_SHIFT:
        return L"Shift";

    case VK_LSHIFT:
        return L"Left Shift";

    case VK_RSHIFT:
        return L"Right Shift";

    case VK_CONTROL:
        return L"Ctrl";

    case VK_LCONTROL:
        return L"Left Ctrl";

    case VK_RCONTROL:
        return L"Right Ctrl";

    case VK_MENU:
        return L"Alt";

    case VK_LMENU:
        return L"Left Alt";

    case VK_RMENU:
        return L"Right Alt";

    case VK_CAPITAL:
        return L"Caps Lock";

    case VK_INSERT:
        return L"Insert";

    case VK_DELETE:
        return L"Delete";

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

    case VK_NUMLOCK:
        return L"Num Lock";

    case VK_SCROLL:
        return L"Scroll Lock";

    case VK_LWIN:
        return L"Left Windows";

    case VK_RWIN:
        return L"Right Windows";

    case VK_APPS:
        return L"Menu";

    case VK_F1:
        return L"F1";

    case VK_F2:
        return L"F2";

    case VK_F3:
        return L"F3";

    case VK_F4:
        return L"F4";

    case VK_F5:
        return L"F5";

    case VK_F6:
        return L"F6";

    case VK_F7:
        return L"F7";

    case VK_F8:
        return L"F8";

    case VK_F9:
        return L"F9";

    case VK_F10:
        return L"F10";

    case VK_F11:
        return L"F11";

    case VK_F12:
        return L"F12";

    case VK_NUMPAD0:
        return L"Num0";

    case VK_NUMPAD1:
        return L"Num1";

    case VK_NUMPAD2:
        return L"Num2";

    case VK_NUMPAD3:
        return L"Num3";

    case VK_NUMPAD4:
        return L"Num4";

    case VK_NUMPAD5:
        return L"Num5";

    case VK_NUMPAD6:
        return L"Num6";

    case VK_NUMPAD7:
        return L"Num7";

    case VK_NUMPAD8:
        return L"Num8";

    case VK_NUMPAD9:
        return L"Num9";

    case VK_ADD:
        return L"Num +";

    case VK_SUBTRACT:
        return L"Num -";

    case VK_MULTIPLY:
        return L"Num *";

    case VK_DIVIDE:
        return L"Num /";

    case VK_DECIMAL:
        return L"Num .";

    default:
        break;
    }

    if (
        vk >= 'A' &&
        vk <= 'Z'
    )
    {
        wchar_t buffer[2] = {
            static_cast<wchar_t>(vk),
            L'\0'
        };

        return buffer;
    }

    if (
        vk >= '0' &&
        vk <= '9'
    )
    {
        wchar_t buffer[2] = {
            static_cast<wchar_t>(vk),
            L'\0'
        };

        return buffer;
    }

    UINT scanCode =
        MapVirtualKeyW(
            vk,
            MAPVK_VK_TO_VSC
        );

    LONG lParam =
        static_cast<LONG>(
            scanCode << 16
        );

    wchar_t buffer[128] = {};

    if (
        GetKeyNameTextW(
            lParam,
            buffer,
            128
        ) > 0
    )
    {
        return buffer;
    }

    std::wstringstream ss;

    ss << L"VK " << vk;

    return ss.str();
}


// ============================================================
// MOUSE BUTTON NAME
// ============================================================

std::wstring GetMouseButtonName(
    WPARAM wParam
)
{
    switch (wParam)
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
    {
        WORD button =
            GET_XBUTTON_WPARAM(
                wParam
            );

        if (button == XBUTTON1)
            return L"X1";

        return L"X2";
    }

    default:
        return L"Mouse";
    }
}


// ============================================================
// KEYBOARD HOOK
// ============================================================

LRESULT CALLBACK KeyboardProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* data =
            reinterpret_cast<KBDLLHOOKSTRUCT*>(
                lParam
            );

        if (data)
        {
            if (!(data->flags & LLKHF_INJECTED))
            {
                DWORD vk =
                    data->vkCode;

                // F8 and F9 remain ignored.
                // F10 is a normal tracked key.
                if (
                    vk != VK_F8 &&
                    vk != VK_F9 &&
                    vk < 256
                )
                {
                    if (
                        wParam == WM_KEYDOWN ||
                        wParam == WM_SYSKEYDOWN
                    )
                    {
                        if (
                            keyRepeatCount[vk] < 5
                        )
                        {
                            keyRepeatCount[vk]++;

                            Action(
                                GetKeyName(vk)
                            );
                        }
                    }
                    else if (
                        wParam == WM_KEYUP ||
                        wParam == WM_SYSKEYUP
                    )
                    {
                        std::wstring name =
                            GetKeyName(vk);

                        EnterCriticalSection(
                            &actionLock
                        );

                        RecordReleaseLocked(
                            name
                        );

                        keyRepeatCount[vk] = 0;

                        LeaveCriticalSection(
                            &actionLock
                        );
                    }
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

LRESULT CALLBACK MouseProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (nCode == HC_ACTION)
    {
        MSLLHOOKSTRUCT* data =
            reinterpret_cast<MSLLHOOKSTRUCT*>(
                lParam
            );

        if (data)
        {
            if (!(data->flags & LLMHF_INJECTED))
            {
                switch (wParam)
                {
                case WM_LBUTTONDOWN:
                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN:
                case WM_XBUTTONDOWN:

                    Action(
                        GetMouseButtonName(
                            wParam
                        )
                    );

                    break;


                case WM_LBUTTONUP:
                case WM_RBUTTONUP:
                case WM_MBUTTONUP:
                case WM_XBUTTONUP:
                {
                    std::wstring name =
                        GetMouseButtonName(
                            wParam
                        );

                    EnterCriticalSection(
                        &actionLock
                    );

                    RecordReleaseLocked(
                        name
                    );

                    LeaveCriticalSection(
                        &actionLock
                    );

                    break;
                }
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
// DRAW TEXT
// ============================================================

void DrawStyledText(
    HDC dc,
    const std::wstring& text,
    int x,
    int y,
    int fontHeight
)
{
    HFONT font =
        CreateFontW(
            -fontHeight * RENDER_SCALE,
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
            L"Arial Narrow"
        );

    HFONT oldFont =
        static_cast<HFONT>(
            SelectObject(
                dc,
                font
            )
        );

    SetBkMode(
        dc,
        TRANSPARENT
    );

    // Dark outline.
    SetTextColor(
        dc,
        RGB(0, 0, 0)
    );

    for (int ox = -2; ox <= 2; ++ox)
    {
        for (int oy = -2; oy <= 2; ++oy)
        {
            TextOutW(
                dc,
                x + ox,
                y + oy,
                text.c_str(),
                static_cast<int>(
                    text.length()
                )
            );
        }
    }

    // Main text.
    SetTextColor(
        dc,
        RGB(255, 255, 255)
    );

    TextOutW(
        dc,
        x,
        y,
        text.c_str(),
        static_cast<int>(
            text.length()
        )
    );

    SelectObject(
        dc,
        oldFont
    );

    DeleteObject(
        font
    );
}


// ============================================================
// DRAW OVERLAY
// ============================================================

void DrawOverlay(
    HDC targetDC
)
{
    int width =
        overlayWidth;

    int height =
        overlayHeight;

    if (
        width <= 0 ||
        height <= 0
    )
        return;

    int scaledWidth =
        width * RENDER_SCALE;

    int scaledHeight =
        height * RENDER_SCALE;

    HDC highDC =
        CreateCompatibleDC(
            targetDC
        );

    BITMAPINFO bmi = {};

    bmi.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);

    bmi.bmiHeader.biWidth =
        scaledWidth;

    bmi.bmiHeader.biHeight =
        -scaledHeight;

    bmi.bmiHeader.biPlanes =
        1;

    bmi.bmiHeader.biBitCount =
        32;

    bmi.bmiHeader.biCompression =
        BI_RGB;

    void* bits = nullptr;

    HBITMAP bitmap =
        CreateDIBSection(
            targetDC,
            &bmi,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0
        );

    if (!bitmap)
    {
        DeleteDC(highDC);
        return;
    }

    HBITMAP oldBitmap =
        static_cast<HBITMAP>(
            SelectObject(
                highDC,
                bitmap
            )
        );

    RECT highRect = {
        0,
        0,
        scaledWidth,
        scaledHeight
    };

    HBRUSH background =
        CreateSolidBrush(
            RGB(0, 0, 0)
        );

    FillRect(
        highDC,
        &highRect,
        background
    );

    DeleteObject(
        background
    );

    int apm =
        GetAPM();

    std::wstring text =
        L"APM " +
        std::to_wstring(
            apm
        );

    int fontHeight =
        static_cast<int>(
            height * 0.62
        );

    if (fontHeight < 10)
        fontHeight = 10;

    DrawStyledText(
        highDC,
        text,
        8 * RENDER_SCALE,
        2 * RENDER_SCALE,
        fontHeight
    );

    HDC finalDC =
        CreateCompatibleDC(
            targetDC
        );

    BITMAPINFO finalBmi = {};

    finalBmi.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);

    finalBmi.bmiHeader.biWidth =
        width;

    finalBmi.bmiHeader.biHeight =
        -height;

    finalBmi.bmiHeader.biPlanes =
        1;

    finalBmi.bmiHeader.biBitCount =
        32;

    finalBmi.bmiHeader.biCompression =
        BI_RGB;

    void* finalBits = nullptr;

    HBITMAP finalBitmap =
        CreateDIBSection(
            targetDC,
            &finalBmi,
            DIB_RGB_COLORS,
            &finalBits,
            nullptr,
            0
        );

    if (finalBitmap)
    {
        HBITMAP oldFinal =
            static_cast<HBITMAP>(
                SelectObject(
                    finalDC,
                    finalBitmap
                )
            );

        BYTE* src =
            static_cast<BYTE*>(
                bits
            );

        BYTE* dst =
            static_cast<BYTE*>(
                finalBits
            );

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                int sx =
                    x * RENDER_SCALE;

                int sy =
                    y * RENDER_SCALE;

                unsigned int b = 0;
                unsigned int g = 0;
                unsigned int r = 0;

                for (
                    int yy = 0;
                    yy < RENDER_SCALE;
                    ++yy
                )
                {
                    for (
                        int xx = 0;
                        xx < RENDER_SCALE;
                        ++xx
                    )
                    {
                        BYTE* pixel =
                            src +
                            (
                                (sy + yy) *
                                scaledWidth +
                                (sx + xx)
                            ) * 4;

                        b += pixel[0];
                        g += pixel[1];
                        r += pixel[2];
                    }
                }

                int samples =
                    RENDER_SCALE *
                    RENDER_SCALE;

                BYTE* out =
                    dst +
                    (
                        (y * width + x)
                        * 4
                    );

                out[0] =
                    static_cast<BYTE>(
                        b / samples
                    );

                out[1] =
                    static_cast<BYTE>(
                        g / samples
                    );

                out[2] =
                    static_cast<BYTE>(
                        r / samples
                    );

                out[3] = 255;
            }
        }

        BitBlt(
            targetDC,
            0,
            0,
            width,
            height,
            finalDC,
            0,
            0,
            SRCCOPY
        );

        SelectObject(
            finalDC,
            oldFinal
        );

        DeleteObject(
            finalBitmap
        );
    }

    DeleteDC(
        finalDC
    );

    SelectObject(
        highDC,
        oldBitmap
    );

    DeleteObject(
        bitmap
    );

    DeleteDC(
        highDC
    );
}


// ============================================================
// CLICKABLE MODE
// ============================================================

void SetClickableMode(
    bool clickable
)
{
    clickableMode =
        clickable;

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
    }
    else
    {
        style &=
            ~WS_THICKFRAME;

        exStyle |=
            WS_EX_TRANSPARENT;
    }

    exStyle |=
        WS_EX_LAYERED |
        WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW;

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
        overlayX,
        overlayY,
        overlayWidth,
        overlayHeight,
        SWP_FRAMECHANGED |
        SWP_NOACTIVATE |
        SWP_SHOWWINDOW
    );

    InvalidateRect(
        hwnd,
        nullptr,
        TRUE
    );
}


// ============================================================
// TRAY ICON
// ============================================================

void AddTrayIcon()
{
    NOTIFYICONDATAW nid = {};

    nid.cbSize =
        sizeof(nid);

    nid.hWnd =
        hwnd;

    nid.uID = 1;

    nid.uFlags =
        NIF_MESSAGE |
        NIF_ICON |
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
    NOTIFYICONDATAW nid = {};

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


// ============================================================
// TRAY MENU
// ============================================================

void ShowTrayMenu()
{
    HMENU menu =
        CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_CLICK,
        clickableMode
            ? L"Pass-through Mode"
            : L"Clickable Mode"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_RESET,
        L"Reset APM"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_INPUT,
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

    POINT point;

    GetCursorPos(
        &point
    );

    SetForegroundWindow(
        hwnd
    );

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON |
        TPM_BOTTOMALIGN,
        point.x,
        point.y,
        0,
        hwnd,
        nullptr
    );

    DestroyMenu(
        menu
    );
}


// ============================================================
// WINDOW PROCEDURE
// ============================================================

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

        if (
            lParam == WM_RBUTTONUP ||
            lParam == WM_CONTEXTMENU
        )
        {
            ShowTrayMenu();
        }

        return 0;


    case WM_COMMAND:

        switch (LOWORD(wParam))
        {
        case ID_TRAY_CLICK:

            SetClickableMode(
                !clickableMode
            );

            return 0;


        case ID_TRAY_RESET:

            // APM ONLY.
            ResetAPM();

            return 0;


        case ID_TRAY_INPUT:

            // INPUT TRACKER ONLY.
            ResetInputTracker();

            return 0;


        case ID_TRAY_EXIT:

            DestroyWindow(
                hwnd
            );

            return 0;
        }

        break;


    case WM_NCHITTEST:

        if (!clickableMode)
            return HTTRANSPARENT;

        {
            POINT pt = {
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)
            };

            ScreenToClient(
                window,
                &pt
            );

            RECT rect;

            GetClientRect(
                window,
                &rect
            );

            const int grip = 8;

            bool left =
                pt.x <= grip;

            bool right =
                pt.x >=
                rect.right - grip;

            bool top =
                pt.y <= grip;

            bool bottom =
                pt.y >=
                rect.bottom - grip;

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
                return HTCAPTION;

            if (bottom)
                return HTBOTTOM;

            return HTCLIENT;
        }


    case WM_MOVE:

        overlayX =
            static_cast<int>(
                static_cast<short>(
                    LOWORD(lParam)
                )
            );

        overlayY =
            static_cast<int>(
                static_cast<short>(
                    HIWORD(lParam)
                )
            );

        SaveSettings();

        return 0;


    case WM_SIZE:

        if (
            wParam != SIZE_MINIMIZED
        )
        {
            overlayWidth =
                LOWORD(lParam);

            overlayHeight =
                HIWORD(lParam);

            if (overlayWidth < 50)
                overlayWidth = 50;

            if (overlayHeight < 20)
                overlayHeight = 20;

            SaveSettings();
        }

        return 0;


    case WM_TIMER:

        if (wParam == 1)
        {
            EnterCriticalSection(
                &actionLock
            );

            RemoveOldActionsLocked();

            LeaveCriticalSection(
                &actionLock
            );

            InvalidateRect(
                window,
                nullptr,
                FALSE
            );
        }

        return 0;


    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        HDC dc =
            BeginPaint(
                window,
                &ps
            );

        DrawOverlay(
            dc
        );

        if (clickableMode)
        {
            RECT bar = {
                0,
                0,
                overlayWidth,
                TOP_BAR_HEIGHT
            };

            HBRUSH brush =
                CreateSolidBrush(
                    RGB(255, 255, 255)
                );

            FillRect(
                dc,
                &bar,
                brush
            );

            DeleteObject(
                brush
            );
        }

        EndPaint(
            window,
            &ps
        );

        return 0;
    }


    case WM_ERASEBKGND:

        return 1;


    case WM_DESTROY:

        SaveSettings();

        SaveInputStats();

        KillTimer(
            window,
            1
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

        RemoveTrayIcon();

        PostQuitMessage(
            0
        );

        return 0;
    }

    return DefWindowProcW(
        window,
        message,
        wParam,
        lParam
    );
}


// ============================================================
// WINMAIN
// ============================================================

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

    LoadSettings();

    const wchar_t CLASS_NAME[] =
        L"APMOverlayWindow";

    WNDCLASSW wc = {};

    wc.lpfnWndProc =
        WndProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        CLASS_NAME;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        nullptr;

    RegisterClassW(
        &wc
    );

    hwnd =
        CreateWindowExW(
            WS_EX_LAYERED |
            WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW |
            WS_EX_TRANSPARENT,
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

    SetLayeredWindowAttributes(
        hwnd,
        RGB(0, 0, 0),
        0,
        LWA_COLORKEY
    );

    AddTrayIcon();

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

    // Start in pass-through mode.
    SetClickableMode(
        false
    );

    // Update APM once per second.
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

    UpdateWindow(
        hwnd
    );

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
        &actionLock
    );

    return 0;
}
