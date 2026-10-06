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
// GLOBALS
// ============================================================

static HWND hwnd = nullptr;

static HHOOK keyboardHook = nullptr;
static HHOOK mouseHook = nullptr;

static bool clickableMode = false;

static int overlayX = 20;
static int overlayY = 20;
static int overlayWidth = 150;
static int overlayHeight = 45;

static const int TOP_BAR_HEIGHT = 6;

static CRITICAL_SECTION actionLock;

static std::vector<ULONGLONG> actions;

// Guarantees that APM eventually reaches zero.
static ULONGLONG lastActionTime = 0;

// Keyboard repeat tracking.
static BYTE keyRepeatCount[256] = { 0 };

// ============================================================
// INPUT STATISTICS
// ============================================================

struct InputStat
{
    std::wstring name;
    unsigned long long count;
};

static std::vector<InputStat> inputStats;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================

void SaveSettings();
void LoadSettings();

void SaveInputStats();

void RemoveOldActionsLocked();

void Action(const std::wstring& inputName);

int GetAPM();

void ResetAPM();

void DrawOverlay();

void SetClickableMode(bool enabled);

LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
);

LRESULT CALLBACK KeyboardHookProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
);

LRESULT CALLBACK MouseHookProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
);

// ============================================================
// GET EXE DIRECTORY
// ============================================================

std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH] = {};

    GetModuleFileNameW(
        nullptr,
        path,
        MAX_PATH
    );

    std::wstring fullPath(path);

    size_t pos =
        fullPath.find_last_of(L"\\/");

    if (pos == std::wstring::npos)
        return L".";

    return fullPath.substr(0, pos);
}

// ============================================================
// SETTINGS FILE
// ============================================================

std::wstring GetSettingsPath()
{
    return GetExeDirectory() +
           L"\\APMOverlay.txt";
}

void SaveSettings()
{
    std::ofstream file(
        GetSettingsPath(),
        std::ios::trunc
    );

    if (!file)
        return;

    file << "X=" << overlayX << "\n";
    file << "Y=" << overlayY << "\n";
    file << "Width=" << overlayWidth << "\n";
    file << "Height=" << overlayHeight << "\n";
}

void LoadSettings()
{
    std::ifstream file(
        GetSettingsPath()
    );

    if (!file)
        return;

    std::string line;

    while (std::getline(file, line))
    {
        size_t equal =
            line.find('=');

        if (equal == std::string::npos)
            continue;

        std::string key =
            line.substr(0, equal);

        std::string value =
            line.substr(equal + 1);

        try
        {
            int number =
                std::stoi(value);

            if (key == "X")
                overlayX = number;

            else if (key == "Y")
                overlayY = number;

            else if (key == "Width")
                overlayWidth = number;

            else if (key == "Height")
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
}

// ============================================================
// INPUT STATISTICS FILE
// ============================================================

std::wstring GetInputStatsPath()
{
    return GetExeDirectory() +
           L"\\Inputs.txt";
}

void SaveInputStats()
{
    std::ofstream file(
        GetInputStatsPath(),
        std::ios::trunc
    );

    if (!file)
        return;

    for (const auto& stat : inputStats)
    {
        // Convert wide string to UTF-8.
        int sizeNeeded =
            WideCharToMultiByte(
                CP_UTF8,
                0,
                stat.name.c_str(),
                -1,
                nullptr,
                0,
                nullptr,
                nullptr
            );

        if (sizeNeeded <= 0)
            continue;

        std::string utf8(
            sizeNeeded - 1,
            '\0'
        );

        WideCharToMultiByte(
            CP_UTF8,
            0,
            stat.name.c_str(),
            -1,
            &utf8[0],
            sizeNeeded,
            nullptr,
            nullptr
        );

        file
            << utf8
            << " = "
            << stat.count
            << "\n";
    }
}

void RecordInputLocked(
    const std::wstring& inputName
)
{
    for (auto& stat : inputStats)
    {
        if (stat.name == inputName)
        {
            stat.count++;

            // Update the file immediately.
            SaveInputStats();

            return;
        }
    }

    InputStat newStat;
    newStat.name = inputName;
    newStat.count = 1;

    inputStats.push_back(newStat);

    // Update immediately when a new input appears.
    SaveInputStats();
}

// ============================================================
// APM CLEANUP
// ============================================================

void RemoveOldActionsLocked()
{
    const ULONGLONG now =
        GetTickCount64();

    const ULONGLONG window =
        60000ULL;

    // --------------------------------------------------------
    // HARD ZERO FIX
    //
    // If there has been absolutely no input for 60 seconds,
    // there is no reason for anything to remain in the APM
    // history.
    // --------------------------------------------------------

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

    // --------------------------------------------------------
    // Remove everything older than 60 seconds.
    // --------------------------------------------------------

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

    // Extra safety.
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
    const ULONGLONG now =
        GetTickCount64();

    EnterCriticalSection(
        &actionLock
    );

    actions.push_back(now);

    lastActionTime = now;

    RecordInputLocked(
        inputName
    );

    RemoveOldActionsLocked();

    LeaveCriticalSection(
        &actionLock
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
// RESET APM
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
// KEY NAME
// ============================================================

std::wstring GetKeyName(
    DWORD vkCode
)
{
    switch (vkCode)
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

        case VK_TAB:
            return L"Tab";

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
            return L"Left Arrow";

        case VK_RIGHT:
            return L"Right Arrow";

        case VK_UP:
            return L"Up Arrow";

        case VK_DOWN:
            return L"Down Arrow";

        case VK_NUMLOCK:
            return L"Num Lock";

        case VK_SCROLL:
            return L"Scroll Lock";

        case VK_LWIN:
            return L"Left Win";

        case VK_RWIN:
            return L"Right Win";

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
    }

    // A-Z
    if (
        (vkCode >= 'A' && vkCode <= 'Z')
    )
    {
        wchar_t buffer[2] = {};

        buffer[0] =
            static_cast<wchar_t>(
                vkCode
            );

        return buffer;
    }

    // 0-9
    if (
        vkCode >= '0' &&
        vkCode <= '9'
    )
    {
        wchar_t buffer[2] = {};

        buffer[0] =
            static_cast<wchar_t>(
                vkCode
            );

        return buffer;
    }

    // Numpad
    switch (vkCode)
    {
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

        case VK_MULTIPLY:
            return L"Num *";

        case VK_ADD:
            return L"Num +";

        case VK_SUBTRACT:
            return L"Num -";

        case VK_DECIMAL:
            return L"Num .";

        case VK_DIVIDE:
            return L"Num /";
    }

    // OEM keys
    switch (vkCode)
    {
        case VK_OEM_1:
            return L";";

        case VK_OEM_PLUS:
            return L"=";

        case VK_OEM_COMMA:
            return L",";

        case VK_OEM_MINUS:
            return L"-";

        case VK_OEM_PERIOD:
            return L".";

        case VK_OEM_2:
            return L"/";

        case VK_OEM_3:
            return L"`";

        case VK_OEM_4:
            return L"[";

        case VK_OEM_5:
            return L"\\";

        case VK_OEM_6:
            return L"]";

        case VK_OEM_7:
            return L"'";
    }

    // Fallback
    wchar_t name[64] = {};

    UINT scanCode =
        MapVirtualKeyW(
            vkCode,
            MAPVK_VK_TO_VSC
        );

    LONG lParam =
        static_cast<LONG>(
            scanCode << 16
        );

    if (GetKeyNameTextW(
        lParam,
        name,
        64
    ))
    {
        return name;
    }

    std::wstringstream ss;

    ss << L"VK "
       << vkCode;

    return ss.str();
}

// ============================================================
// KEYBOARD HOOK
// ============================================================

LRESULT CALLBACK KeyboardHookProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* kb =
            reinterpret_cast<KBDLLHOOKSTRUCT*>(
                lParam
            );

        // Ignore injected/synthetic keyboard input.
        if (
            kb &&
            !(kb->flags & LLKHF_INJECTED)
        )
        {
            DWORD vk =
                kb->vkCode;

            // Ignore F8 and F9.
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
                    // Windows can generate repeated
                    // keydown events while holding a key.
                    //
                    // First press + maximum 5 repeats.
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
                    keyRepeatCount[vk] = 0;
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
// MOUSE BUTTON NAME
// ============================================================

std::wstring GetMouseButtonName(
    WPARAM message
)
{
    switch (message)
    {
        case WM_LBUTTONDOWN:
            return L"Left Click";

        case WM_RBUTTONDOWN:
            return L"Right Click";

        case WM_MBUTTONDOWN:
            return L"Middle Click";

        case WM_XBUTTONDOWN:
            return L"X Button";

        default:
            return L"Mouse";
    }
}

// ============================================================
// MOUSE HOOK
// ============================================================

LRESULT CALLBACK MouseHookProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam
)
{
    if (nCode == HC_ACTION)
    {
        MSLLHOOKSTRUCT* ms =
            reinterpret_cast<MSLLHOOKSTRUCT*>(
                lParam
            );

        // Ignore injected/synthetic mouse input.
        if (
            ms &&
            !(ms->flags & LLMHF_INJECTED)
        )
        {
            switch (wParam)
            {
                case WM_LBUTTONDOWN:
                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN:
                case WM_XBUTTONDOWN:
                {
                    Action(
                        GetMouseButtonName(
                            wParam
                        )
                    );

                    break;
                }

                default:
                    break;
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
// STYLED TEXT
// ============================================================

void DrawStyledText(
    HDC dc,
    const std::wstring& text,
    RECT rect
)
{
    const int outline = 2;
    const int shadow = 2;

    SetTextCharacterExtra(
        dc,
        1
    );

    SetBkMode(
        dc,
        TRANSPARENT
    );

    // Shadow
    SetTextColor(
        dc,
        RGB(0, 0, 0)
    );

    RECT shadowRect = rect;

    shadowRect.left += shadow;
    shadowRect.top += shadow;
    shadowRect.right += shadow;
    shadowRect.bottom += shadow;

    DrawTextW(
        dc,
        text.c_str(),
        -1,
        &shadowRect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE |
        DT_NOPREFIX
    );

    // Dark outline
    SetTextColor(
        dc,
        RGB(25, 25, 25)
    );

    const int offsets[][2] =
    {
        {-outline, -outline},
        {0,        -outline},
        {outline,  -outline},

        {-outline, 0},
        {outline,  0},

        {-outline, outline},
        {0,        outline},
        {outline,  outline}
    };

    for (
        const auto& offset :
        offsets
    )
    {
        RECT outlineRect = rect;

        outlineRect.left +=
            offset[0];

        outlineRect.top +=
            offset[1];

        outlineRect.right +=
            offset[0];

        outlineRect.bottom +=
            offset[1];

        DrawTextW(
            dc,
            text.c_str(),
            -1,
            &outlineRect,
            DT_CENTER |
            DT_VCENTER |
            DT_SINGLELINE |
            DT_NOPREFIX
        );
    }

    // Main text
    SetTextColor(
        dc,
        RGB(245, 245, 245)
    );

    DrawTextW(
        dc,
        text.c_str(),
        -1,
        &rect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE |
        DT_NOPREFIX
    );

    SetTextCharacterExtra(
        dc,
        0
    );
}

// ============================================================
// DRAW OVERLAY
// ============================================================

void DrawOverlay()
{
    if (!hwnd)
        return;

    RECT windowRect;

    if (
        !GetWindowRect(
            hwnd,
            &windowRect
        )
    )
    {
        return;
    }

    overlayX =
        windowRect.left;

    overlayY =
        windowRect.top;

    overlayWidth =
        windowRect.right -
        windowRect.left;

    overlayHeight =
        windowRect.bottom -
        windowRect.top;

    HDC screenDC =
        GetDC(nullptr);

    HDC memDC =
        CreateCompatibleDC(
            screenDC
        );

    BITMAPINFO bmi = {};

    bmi.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);

    bmi.bmiHeader.biWidth =
        overlayWidth;

    bmi.bmiHeader.biHeight =
        -overlayHeight;

    bmi.bmiHeader.biPlanes =
        1;

    bmi.bmiHeader.biBitCount =
        32;

    bmi.bmiHeader.biCompression =
        BI_RGB;

    void* bits = nullptr;

    HBITMAP bitmap =
        CreateDIBSection(
            memDC,
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
    RECT fullRect =
    {
        0,
        0,
        overlayWidth,
        overlayHeight
    };

    FillRect(
        memDC,
        &fullRect,
        (HBRUSH)GetStockObject(
            BLACK_BRUSH
        )
    );

    // --------------------------------------------------------
    // CLICKABLE MODE TOP BAR
    // --------------------------------------------------------

    if (clickableMode)
    {
        RECT barRect =
        {
            0,
            0,
            overlayWidth,
            TOP_BAR_HEIGHT
        };

        HBRUSH whiteBrush =
            CreateSolidBrush(
                RGB(255, 255, 255)
            );

        FillRect(
            memDC,
            &barRect,
            whiteBrush
        );

        DeleteObject(
            whiteBrush
        );
    }

    // --------------------------------------------------------
    // FONT
    // --------------------------------------------------------

    int fontHeight =
        max(
            12,
            overlayHeight -
            TOP_BAR_HEIGHT -
            6
        );

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
            L"Arial Narrow"
        );

    HFONT oldFont =
        (HFONT)SelectObject(
            memDC,
            font
        );

    // --------------------------------------------------------
    // APM TEXT
    // --------------------------------------------------------

    std::wstring text =
        L"APM " +
        std::to_wstring(
            GetAPM()
        );

    RECT textRect =
    {
        0,
        TOP_BAR_HEIGHT,
        overlayWidth,
        overlayHeight
    };

    DrawStyledText(
        memDC,
        text,
        textRect
    );

    // --------------------------------------------------------
    // CREATE ALPHA BITMAP
    // --------------------------------------------------------

    if (bits)
    {
        DWORD* pixels =
            static_cast<DWORD*>(
                bits
            );

        const int pixelCount =
            overlayWidth *
            overlayHeight;

        for (
            int i = 0;
            i < pixelCount;
            ++i
        )
        {
            DWORD pixel =
                pixels[i];

            BYTE blue =
                static_cast<BYTE>(
                    pixel & 0xFF
                );

            BYTE green =
                static_cast<BYTE>(
                    (pixel >> 8) & 0xFF
                );

            BYTE red =
                static_cast<BYTE>(
                    (pixel >> 16) & 0xFF
                );

            BYTE brightness =
                max(
                    red,
                    max(
                        green,
                        blue
                    )
                );

            if (brightness == 0)
            {
                pixels[i] = 0;
            }
            else
            {
                pixels[i] =
                    (static_cast<DWORD>(
                        brightness
                    ) << 24) |

                    (static_cast<DWORD>(
                        red
                    ) << 16) |

                    (static_cast<DWORD>(
                        green
                    ) << 8) |

                    static_cast<DWORD>(
                        blue
                    );
            }
        }
    }

    POINT destination =
    {
        overlayX,
        overlayY
    };

    SIZE size =
    {
        overlayWidth,
        overlayHeight
    };

    POINT source =
    {
        0,
        0
    };

    BLENDFUNCTION blend = {};

    blend.BlendOp =
        AC_SRC_OVER;

    blend.SourceConstantAlpha =
        255;

    blend.AlphaFormat =
        AC_SRC_ALPHA;

    UpdateLayeredWindow(
        hwnd,
        screenDC,
        &destination,
        &size,
        memDC,
        &source,
        0,
        &blend,
        ULW_ALPHA
    );

    SelectObject(
        memDC,
        oldFont
    );

    DeleteObject(
        font
    );

    SelectObject(
        memDC,
        oldBitmap
    );

    DeleteObject(
        bitmap
    );

    DeleteDC(
        memDC
    );

    ReleaseDC(
        nullptr,
        screenDC
    );
}

// ============================================================
// CLICKABLE MODE
// ============================================================

void SetClickableMode(
    bool enabled
)
{
    clickableMode =
        enabled;

    LONG_PTR style =
        GetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE
        );

    if (clickableMode)
    {
        style &=
            ~WS_EX_TRANSPARENT;

        style |=
            WS_EX_LAYERED |
            WS_EX_TOPMOST;
    }
    else
    {
        style |=
            WS_EX_TRANSPARENT |
            WS_EX_LAYERED |
            WS_EX_TOPMOST;
    }

    SetWindowLongPtrW(
        hwnd,
        GWL_EXSTYLE,
        style
    );

    LONG_PTR windowStyle =
        GetWindowLongPtrW(
            hwnd,
            GWL_STYLE
        );

    if (clickableMode)
    {
        windowStyle |=
            WS_THICKFRAME;
    }
    else
    {
        windowStyle &=
            ~WS_THICKFRAME;
    }

    SetWindowLongPtrW(
        hwnd,
        GWL_STYLE,
        windowStyle
    );

    SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        overlayX,
        overlayY,
        overlayWidth,
        overlayHeight,
        SWP_NOACTIVATE |
        SWP_FRAMECHANGED |
        SWP_SHOWWINDOW
    );

    DrawOverlay();
}

// ============================================================
// TRAY
// ============================================================

#define WM_TRAYICON     (WM_USER + 1)

#define ID_TRAY_CLICK   1001
#define ID_TRAY_RESET   1002
#define ID_TRAY_EXIT    1003

void ShowTrayMenu()
{
    POINT pt;

    GetCursorPos(
        &pt
    );

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
// WINDOW PROCEDURE
// ============================================================

LRESULT CALLBACK WindowProc(
    HWND window,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (msg)
    {
        case WM_TIMER:
        {
            if (wParam == 1)
            {
                DrawOverlay();
            }

            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;

            BeginPaint(
                window,
                &ps
            );

            EndPaint(
                window,
                &ps
            );

            DrawOverlay();

            return 0;
        }

        case WM_MOVE:
        {
            RECT rect;

            if (
                GetWindowRect(
                    window,
                    &rect
                )
            )
            {
                overlayX =
                    rect.left;

                overlayY =
                    rect.top;

                SaveSettings();
            }

            return 0;
        }

        case WM_SIZE:
        {
            RECT rect;

            if (
                GetWindowRect(
                    window,
                    &rect
                )
            )
            {
                overlayX =
                    rect.left;

                overlayY =
                    rect.top;

                overlayWidth =
                    rect.right -
                    rect.left;

                overlayHeight =
                    rect.bottom -
                    rect.top;

                SaveSettings();
            }

            return 0;
        }

        case WM_NCHITTEST:
        {
            if (!clickableMode)
            {
                return HTTRANSPARENT;
            }

            POINT pt =
            {
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)
            };

            RECT rect;

            GetWindowRect(
                window,
                &rect
            );

            int x =
                pt.x -
                rect.left;

            int y =
                pt.y -
                rect.top;

            const int grip =
                8;

            bool left =
                x < grip;

            bool right =
                x >=
                overlayWidth -
                grip;

            bool top =
                y < grip;

            bool bottom =
                y >=
                overlayHeight -
                grip;

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

            // Allow dragging from the top bar.
            if (
                y >= 0 &&
                y < TOP_BAR_HEIGHT
            )
            {
                return HTCAPTION;
            }

            return HTCLIENT;
        }

        case WM_TRAYICON:
        {
            if (
                lParam ==
                WM_RBUTTONUP
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
                case ID_TRAY_CLICK:
                {
                    SetClickableMode(
                        !clickableMode
                    );

                    return 0;
                }

                case ID_TRAY_RESET:
                {
                    ResetAPM();

                    return 0;
                }

                case ID_TRAY_EXIT:
                {
                    DestroyWindow(
                        window
                    );

                    return 0;
                }
            }

            break;
        }

        case WM_DESTROY:
        {
            SaveSettings();
            SaveInputStats();

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
                window,
                1
            );

            PostQuitMessage(
                0
            );

            return 0;
        }
    }

    return DefWindowProcW(
        window,
        msg,
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
        WindowProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        CLASS_NAME;

    wc.hCursor =
        LoadCursor(
            nullptr,
            IDC_ARROW
        );

    RegisterClassW(
        &wc
    );

    DWORD exStyle =
        WS_EX_LAYERED |
        WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW |
        WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE;

    hwnd =
        CreateWindowExW(
            exStyle,
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

    // --------------------------------------------------------
    // TRAY ICON
    // --------------------------------------------------------

    NOTIFYICONDATAW nid = {};

    nid.cbSize =
        sizeof(nid);

    nid.hWnd =
        hwnd;

    nid.uID =
        1;

    nid.uFlags =
        NIF_ICON |
        NIF_MESSAGE |
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

    // --------------------------------------------------------
    // HOOKS
    // --------------------------------------------------------

    keyboardHook =
        SetWindowsHookExW(
            WH_KEYBOARD_LL,
            KeyboardHookProc,
            hInstance,
            0
        );

    mouseHook =
        SetWindowsHookExW(
            WH_MOUSE_LL,
            MouseHookProc,
            hInstance,
            0
        );

    // --------------------------------------------------------
    // INITIAL MODE
    // --------------------------------------------------------

    SetClickableMode(
        false
    );

    // --------------------------------------------------------
    // REFRESH EVERY SECOND
    // --------------------------------------------------------

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

    DrawOverlay();

    // --------------------------------------------------------
    // MESSAGE LOOP
    // --------------------------------------------------------

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

    NOTIFYICONDATAW removeIcon = {};

    removeIcon.cbSize =
        sizeof(removeIcon);

    removeIcon.hWnd =
        hwnd;

    removeIcon.uID =
        1;

    Shell_NotifyIconW(
        NIM_DELETE,
        &removeIcon
    );

    DeleteCriticalSection(
        &actionLock
    );

    return 0;
}
