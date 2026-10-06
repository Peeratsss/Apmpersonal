#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <mfuuid.h>

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cwctype>
#include <cmath>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// ============================================================
// SETTINGS
// ============================================================

static const int VIDEO_WIDTH = 1920;
static const int VIDEO_HEIGHT = 1080;

static const int FPS = 60;

static const int DEFAULT_WIDTH = 1100;
static const int DEFAULT_HEIGHT = 850;

static const UINT TIMER_PREVIEW = 1;

// ============================================================
// EVENT
// ============================================================

struct TimelineEvent
{
    double time = 0.0;
    std::wstring name;
    bool down = true;
};

// ============================================================
// GLOBALS
// ============================================================

static HWND hwndMain = nullptr;
static HWND hwndTimeline = nullptr;

static HWND hwndLoad = nullptr;
static HWND hwndPlay = nullptr;
static HWND hwndReset = nullptr;
static HWND hwndExport = nullptr;

static std::vector<TimelineEvent> events;

static std::vector<std::wstring> heldKeys;

static size_t processedEventIndex = 0;

static double currentTime = 0.0;
static double duration = 0.0;

static bool playing = false;

static LARGE_INTEGER performanceFrequency{};
static LARGE_INTEGER lastPerformanceTime{};

// ============================================================
// STRING HELPERS
// ============================================================

static std::wstring Trim(
    const std::wstring& input
)
{
    size_t first = 0;

    while (
        first < input.size() &&
        iswspace(input[first])
    )
    {
        ++first;
    }

    size_t last = input.size();

    while (
        last > first &&
        iswspace(input[last - 1])
    )
    {
        --last;
    }

    return input.substr(
        first,
        last - first
    );
}

static std::wstring ToUpper(
    std::wstring value
)
{
    for (wchar_t& c : value)
        c = towupper(c);

    return value;
}

static bool EndsWith(
    const std::wstring& value,
    const std::wstring& ending
)
{
    if (value.size() < ending.size())
        return false;

    return value.compare(
        value.size() - ending.size(),
        ending.size(),
        ending
    ) == 0;
}

// ============================================================
// TIMESTAMP PARSER
// ============================================================

static bool ParseTimestamp(
    const std::wstring& text,
    double& seconds
)
{
    std::wstring s = Trim(text);

    // Expected:
    // 10:46:46.533 PM
    //
    // Also accepts:
    // 10 46 46.533 PM

    for (wchar_t& c : s)
    {
        if (c == ':')
            c = L' ';
    }

    std::wstringstream ss(s);

    int hour = 0;
    int minute = 0;
    double second = 0.0;
    std::wstring ampm;

    if (!(ss >> hour >> minute >> second >> ampm))
        return false;

    ampm = ToUpper(ampm);

    if (ampm != L"AM" &&
        ampm != L"PM")
    {
        return false;
    }

    if (hour < 1 || hour > 12)
        return false;

    if (minute < 0 || minute > 59)
        return false;

    if (second < 0.0 || second >= 60.0)
        return false;

    if (ampm == L"AM")
    {
        if (hour == 12)
            hour = 0;
    }
    else
    {
        if (hour != 12)
            hour += 12;
    }

    seconds =
        static_cast<double>(hour) * 3600.0 +
        static_cast<double>(minute) * 60.0 +
        second;

    return true;
}

// ============================================================
// TIMELINE PARSER
// ============================================================

static bool ParseTimeline(
    const std::wstring& text
)
{
    std::vector<TimelineEvent> parsed;

    std::wstringstream stream(text);
    std::wstring line;

    double previousClock = -1.0;
    double dayOffset = 0.0;

    while (std::getline(stream, line))
    {
        line = Trim(line);

        if (line.empty())
            continue;

        if (line[0] == L'#')
            continue;

        std::wstring timestampText;
        std::wstring inputName;

        size_t tab =
            line.find(L'\t');

        if (tab != std::wstring::npos)
        {
            timestampText =
                Trim(line.substr(0, tab));

            inputName =
                Trim(line.substr(tab + 1));
        }
        else
        {
            // Find AM/PM and use everything before it
            // as timestamp and everything after as input.
            std::wstring upper =
                ToUpper(line);

            size_t amPos =
                upper.find(L" AM");

            size_t pmPos =
                upper.find(L" PM");

            size_t pos = std::wstring::npos;

            if (amPos != std::wstring::npos)
                pos = amPos;

            if (pmPos != std::wstring::npos &&
                (pos == std::wstring::npos ||
                 pmPos < pos))
            {
                pos = pmPos;
            }

            if (pos == std::wstring::npos)
                continue;

            size_t timestampEnd = pos + 3;

            timestampText =
                Trim(line.substr(
                    0,
                    timestampEnd
                ));

            inputName =
                Trim(line.substr(
                    timestampEnd
                ));
        }

        if (timestampText.empty() ||
            inputName.empty())
        {
            continue;
        }

        double clockSeconds = 0.0;

        if (!ParseTimestamp(
                timestampText,
                clockSeconds))
        {
            continue;
        }

        // Handle midnight rollover.
        if (previousClock >= 0.0 &&
            clockSeconds < previousClock)
        {
            dayOffset += 86400.0;
        }

        previousClock = clockSeconds;

        TimelineEvent event;

        event.time =
            clockSeconds + dayOffset;

        event.name = inputName;

        if (EndsWith(
                event.name,
                L"_UP"))
        {
            event.down = false;

            event.name.resize(
                event.name.size() - 3
            );

            event.name =
                Trim(event.name);
        }
        else
        {
            event.down = true;
        }

        parsed.push_back(event);
    }

    if (parsed.empty())
        return false;

    double firstTime =
        parsed.front().time;

    for (TimelineEvent& event : parsed)
    {
        event.time -= firstTime;

        if (event.time < 0.0)
            event.time = 0.0;
    }

    std::stable_sort(
        parsed.begin(),
        parsed.end(),
        [](const TimelineEvent& a,
           const TimelineEvent& b)
        {
            return a.time < b.time;
        }
    );

    events = parsed;

    duration =
        events.back().time + 0.5;

    if (duration < 0.5)
        duration = 0.5;

    currentTime = 0.0;
    processedEventIndex = 0;
    heldKeys.clear();

    return true;
}

// ============================================================
// EDIT CONTROL
// ============================================================

static std::wstring GetEditText(
    HWND edit
)
{
    int length =
        GetWindowTextLengthW(edit);

    if (length <= 0)
        return L"";

    std::wstring text(
        static_cast<size_t>(length) + 1,
        L'\0'
    );

    GetWindowTextW(
        edit,
        &text[0],
        length + 1
    );

    text.resize(
        static_cast<size_t>(length)
    );

    return text;
}

// ============================================================
// HELD STATE
// ============================================================

static bool IsHeld(
    const std::wstring& name
)
{
    return std::find(
        heldKeys.begin(),
        heldKeys.end(),
        name
    ) != heldKeys.end();
}

static void SetHeld(
    const std::wstring& name,
    bool held
)
{
    auto it =
        std::find(
            heldKeys.begin(),
            heldKeys.end(),
            name
        );

    if (held)
    {
        if (it == heldKeys.end())
            heldKeys.push_back(name);
    }
    else
    {
        if (it != heldKeys.end())
            heldKeys.erase(it);
    }
}

// ============================================================
// PROCESS EVENTS
// ============================================================

static void ResetPlaybackState()
{
    heldKeys.clear();
    processedEventIndex = 0;
}

static void ProcessEventsTo(
    double targetTime
)
{
    while (
        processedEventIndex < events.size() &&
        events[processedEventIndex].time <= targetTime
    )
    {
        const TimelineEvent& event =
            events[processedEventIndex];

        SetHeld(
            event.name,
            event.down
        );

        ++processedEventIndex;
    }
}

// ============================================================
// APM
// ============================================================

static int CalculateAPM(
    double time
)
{
    double start =
        time - 60.0;

    int count = 0;

    for (const TimelineEvent& event : events)
    {
        if (!event.down)
            continue;

        if (event.time > time)
            break;

        if (event.time >= start)
            ++count;
    }

    return count;
}

// ============================================================
// DRAW HELPERS
// ============================================================

static void FillRectColor(
    HDC hdc,
    int left,
    int top,
    int right,
    int bottom,
    COLORREF color
)
{
    RECT r{
        left,
        top,
        right,
        bottom
    };

    HBRUSH brush =
        CreateSolidBrush(color);

    FillRect(
        hdc,
        &r,
        brush
    );

    DeleteObject(brush);
}

static void DrawCenteredText(
    HDC hdc,
    const std::wstring& text,
    int left,
    int top,
    int right,
    int bottom,
    int fontSize,
    bool bold
)
{
    HFONT font =
        CreateFontW(
            -fontSize,
            0,
            0,
            0,
            bold ? FW_BOLD : FW_NORMAL,
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

    HFONT old =
        static_cast<HFONT>(
            SelectObject(
                hdc,
                font
            )
        );

    SetBkMode(
        hdc,
        TRANSPARENT
    );

    SetTextColor(
        hdc,
        RGB(255, 255, 255)
    );

    RECT r{
        left,
        top,
        right,
        bottom
    };

    DrawTextW(
        hdc,
        text.c_str(),
        -1,
        &r,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(
        hdc,
        old
    );

    DeleteObject(font);
}

// ============================================================
// KEY DRAWING
// ============================================================

static void DrawKey(
    HDC hdc,
    const std::wstring& name,
    int x,
    int y,
    int width,
    int height,
    int fontSize = 18
)
{
    bool pressed =
        IsHeld(name);

    COLORREF background =
        pressed
        ? RGB(255, 170, 40)
        : RGB(55, 55, 60);

    FillRectColor(
        hdc,
        x,
        y,
        x + width,
        y + height,
        background
    );

    HPEN pen =
        CreatePen(
            PS_SOLID,
            2,
            RGB(100, 100, 105)
        );

    HGDIOBJ oldPen =
        SelectObject(hdc, pen);

    HGDIOBJ oldBrush =
        SelectObject(
            hdc,
            GetStockObject(NULL_BRUSH)
        );

    Rectangle(
        hdc,
        x,
        y,
        x + width,
        y + height
    );

    SelectObject(
        hdc,
        oldBrush
    );

    SelectObject(
        hdc,
        oldPen
    );

    DeleteObject(pen);

    DrawCenteredText(
        hdc,
        name,
        x,
        y,
        x + width,
        y + height,
        fontSize,
        true
    );
}

// ============================================================
// KEYBOARD
// ============================================================

static void DrawKeyboard(
    HDC hdc
)
{
    const int keyW = 70;
    const int keyH = 62;
    const int gap = 8;

    const int startX = 300;
    const int startY = 390;

    // Number row
    const wchar_t* numbers[] =
    {
        L"1", L"2", L"3", L"4", L"5",
        L"6", L"7", L"8", L"9", L"0"
    };

    for (int i = 0; i < 10; ++i)
    {
        DrawKey(
            hdc,
            numbers[i],
            startX + i * (keyW + gap),
            startY,
            keyW,
            keyH
        );
    }

    // Q row
    const wchar_t* qrow[] =
    {
        L"Q", L"W", L"E", L"R", L"T",
        L"Y", L"U", L"I", L"O", L"P"
    };

    for (int i = 0; i < 10; ++i)
    {
        DrawKey(
            hdc,
            qrow[i],
            startX + 30 + i * (keyW + gap),
            startY + keyH + gap,
            keyW,
            keyH
        );
    }

    // A row
    const wchar_t* arow[] =
    {
        L"A", L"S", L"D", L"F", L"G",
        L"H", L"J", L"K", L"L"
    };

    for (int i = 0; i < 9; ++i)
    {
        DrawKey(
            hdc,
            arow[i],
            startX + 65 + i * (keyW + gap),
            startY + 2 * (keyH + gap),
            keyW,
            keyH
        );
    }

    // Z row
    const wchar_t* zrow[] =
    {
        L"Z", L"X", L"C", L"V", L"B",
        L"N", L"M"
    };

    for (int i = 0; i < 7; ++i)
    {
        DrawKey(
            hdc,
            zrow[i],
            startX + 105 + i * (keyW + gap),
            startY + 3 * (keyH + gap),
            keyW,
            keyH
        );
    }

    // Space
    DrawKey(
        hdc,
        L"Space",
        startX + 180,
        startY + 4 * (keyH + gap),
        500,
        keyH
    );

    // Modifiers
    DrawKey(
        hdc,
        L"Left Ctrl",
        startX - 110,
        startY + 4 * (keyH + gap),
        110,
        keyH,
        15
    );

    DrawKey(
        hdc,
        L"Left Shift",
        startX - 30,
        startY + 3 * (keyH + gap),
        125,
        keyH,
        15
    );

    DrawKey(
        hdc,
        L"Left Alt",
        startX + 55,
        startY + 4 * (keyH + gap),
        110,
        keyH,
        15
    );

    DrawKey(
        hdc,
        L"Enter",
        startX + 9 * (keyW + gap) - 15,
        startY + 2 * (keyH + gap),
        125,
        keyH,
        16
    );
}

// ============================================================
// MOUSE
// ============================================================

static void DrawMouse(
    HDC hdc
)
{
    const int x = 1250;
    const int y = 430;

    bool left =
        IsHeld(L"LMB");

    bool right =
        IsHeld(L"RMB");

    FillRectColor(
        hdc,
        x,
        y,
        x + 250,
        y + 330,
        RGB(45, 45, 50)
    );

    HPEN pen =
        CreatePen(
            PS_SOLID,
            4,
            RGB(120, 120, 125)
        );

    HGDIOBJ oldPen =
        SelectObject(
            hdc,
            pen
        );

    HGDIOBJ oldBrush =
        SelectObject(
            hdc,
            GetStockObject(NULL_BRUSH)
        );

    RoundRect(
        hdc,
        x,
        y,
        x + 250,
        y + 330,
        70,
        70
    );

    SelectObject(
        hdc,
        oldBrush
    );

    SelectObject(
        hdc,
        oldPen
    );

    DeleteObject(pen);

    FillRectColor(
        hdc,
        x + 15,
        y + 15,
        x + 118,
        y + 155,
        left
            ? RGB(255, 170, 40)
            : RGB(70, 70, 75)
    );

    FillRectColor(
        hdc,
        x + 132,
        y + 15,
        x + 235,
        y + 155,
        right
            ? RGB(255, 170, 40)
            : RGB(70, 70, 75)
    );

    DrawCenteredText(
        hdc,
        L"LMB",
        x + 15,
        y + 30,
        x + 118,
        y + 140,
        25,
        true
    );

    DrawCenteredText(
        hdc,
        L"RMB",
        x + 132,
        y + 30,
        x + 235,
        y + 140,
        25,
        true
    );

    DrawCenteredText(
        hdc,
        L"MOUSE",
        x,
        y + 180,
        x + 250,
        y + 240,
        24,
        true
    );
}

// ============================================================
// SCENE
// ============================================================

static void RenderScene(
    HDC hdc,
    int width,
    int height
)
{
    FillRectColor(
        hdc,
        0,
        0,
        width,
        height,
        RGB(18, 18, 22)
    );

    int apm =
        CalculateAPM(currentTime);

    wchar_t apmText[64];

    swprintf_s(
        apmText,
        L"APM %d",
        apm
    );

    DrawCenteredText(
        hdc,
        apmText,
        0,
        50,
        width,
        160,
        70,
        true
    );

    wchar_t timeText[64];

    swprintf_s(
        timeText,
        L"%02d:%05.2f",
        static_cast<int>(currentTime / 60.0),
        fmod(currentTime, 60.0)
    );

    DrawCenteredText(
        hdc,
        timeText,
        0,
        165,
        width,
        215,
        26,
        false
    );

    DrawKeyboard(hdc);
    DrawMouse(hdc);

    // Progress bar
    const int barX = 250;
    const int barY = 920;
    const int barW = 1420;
    const int barH = 18;

    FillRectColor(
        hdc,
        barX,
        barY,
        barX + barW,
        barY + barH,
        RGB(50, 50, 55)
    );

    double progress =
        duration > 0.0
        ? currentTime / duration
        : 0.0;

    progress =
        std::max(
            0.0,
            std::min(
                1.0,
                progress
            )
        );

    FillRectColor(
        hdc,
        barX,
        barY,
        barX +
            static_cast<int>(
                barW * progress
            ),
        barY + barH,
        RGB(255, 170, 40)
    );
}

// ============================================================
// PREVIEW
// ============================================================

static void DrawPreview(
    HDC hdc,
    RECT rc
)
{
    ResetPlaybackState();

    ProcessEventsTo(currentTime);

    RenderScene(
        hdc,
        rc.right - rc.left,
        rc.bottom - rc.top
    );
}

// ============================================================
// LOAD
// ============================================================

static void LoadTimelineFromEditor()
{
    std::wstring text =
        GetEditText(hwndTimeline);

    if (!ParseTimeline(text))
    {
        MessageBoxW(
            hwndMain,
            L"No valid timeline events were found.\n\n"
            L"Example:\n"
            L"10:46:46.533 PM    1\n"
            L"10:46:46.638 PM    2\n"
            L"10:46:46.654 PM    1_UP",
            L"Timeline Error",
            MB_OK |
            MB_ICONWARNING
        );

        return;
    }

    playing = false;

    SetWindowTextW(
        hwndPlay,
        L"Play"
    );

    InvalidateRect(
        hwndMain,
        nullptr,
        TRUE
    );
}

// ============================================================
// EXPORT HELPERS
// ============================================================

static HBITMAP CreateVideoBitmap(
    HDC* outDC,
    void** outBits
)
{
    HDC screen =
        GetDC(nullptr);

    HDC memDC =
        CreateCompatibleDC(screen);

    BITMAPINFO bmi{};

    bmi.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);

    bmi.bmiHeader.biWidth =
        VIDEO_WIDTH;

    bmi.bmiHeader.biHeight =
        -VIDEO_HEIGHT;

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

    ReleaseDC(
        nullptr,
        screen
    );

    if (!bitmap)
    {
        DeleteDC(memDC);
        return nullptr;
    }

    *outDC = memDC;
    *outBits = bits;

    return bitmap;
}

// ============================================================
// EXPORT MP4
// ============================================================

static bool ExportMP4()
{
    if (events.empty())
    {
        MessageBoxW(
            hwndMain,
            L"Load a timeline first.",
            L"Export MP4",
            MB_OK |
            MB_ICONWARNING
        );

        return false;
    }

    std::wstring outputPath =
        []()
        {
            wchar_t path[MAX_PATH]{};

            GetModuleFileNameW(
                nullptr,
                path,
                MAX_PATH
            );

            std::wstring result(path);

            size_t slash =
                result.find_last_of(
                    L"\\/"
                );

            if (slash != std::wstring::npos)
                result.resize(slash);

            return result +
                L"\\APM_Replay.mp4";
        }();

    HRESULT hr =
        MFStartup(
            MF_VERSION,
            MFSTARTUP_FULL
        );

    if (FAILED(hr))
        return false;

    IMFAttributes* attributes = nullptr;
    IMFSinkWriter* writer = nullptr;
    IMFMediaType* outputType = nullptr;
    IMFMediaType* inputType = nullptr;

    bool success = false;

    do
    {
        hr =
            MFCreateAttributes(
                &attributes,
                4
            );

        if (FAILED(hr))
            break;

        hr =
            attributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                TRUE
            );

        if (FAILED(hr))
            break;

        hr =
            MFCreateSinkWriterFromURL(
                outputPath.c_str(),
                nullptr,
                attributes,
                &writer
            );

        if (FAILED(hr))
            break;

        // Output H.264
        hr =
            MFCreateMediaType(
                &outputType
            );

        if (FAILED(hr))
            break;

        outputType->SetGUID(
            MF_MT_MAJOR_TYPE,
            MFMediaType_Video
        );

        outputType->SetGUID(
            MF_MT_SUBTYPE,
            MFVideoFormat_H264
        );

        MFSetAttributeSize(
            outputType,
            MF_MT_FRAME_SIZE,
            VIDEO_WIDTH,
            VIDEO_HEIGHT
        );

        MFSetAttributeRatio(
            outputType,
            MF_MT_FRAME_RATE,
            FPS,
            1
        );

        MFSetAttributeRatio(
            outputType,
            MF_MT_PIXEL_ASPECT_RATIO,
            1,
            1
        );

        outputType->SetUINT32(
            MF_MT_AVG_BITRATE,
            12000000
        );

        DWORD streamIndex = 0;

        hr =
            writer->AddStream(
                outputType,
                &streamIndex
            );

        if (FAILED(hr))
            break;

        // Input RGB32
        hr =
            MFCreateMediaType(
                &inputType
            );

        if (FAILED(hr))
            break;

        inputType->SetGUID(
            MF_MT_MAJOR_TYPE,
            MFMediaType_Video
        );

        inputType->SetGUID(
            MF_MT_SUBTYPE,
            MFVideoFormat_RGB32
        );

        MFSetAttributeSize(
            inputType,
            MF_MT_FRAME_SIZE,
            VIDEO_WIDTH,
            VIDEO_HEIGHT
        );

        MFSetAttributeRatio(
            inputType,
            MF_MT_FRAME_RATE,
            FPS,
            1
        );

        MFSetAttributeRatio(
            inputType,
            MF_MT_PIXEL_ASPECT_RATIO,
            1,
            1
        );

        hr =
            writer->SetInputMediaType(
                streamIndex,
                inputType,
                nullptr
            );

        if (FAILED(hr))
            break;

        hr =
            writer->BeginWriting();

        if (FAILED(hr))
            break;

        HDC videoDC = nullptr;
        void* bits = nullptr;

        HBITMAP bitmap =
            CreateVideoBitmap(
                &videoDC,
                &bits
            );

        if (!bitmap)
            break;

        HBITMAP oldBitmap =
            static_cast<HBITMAP>(
                SelectObject(
                    videoDC,
                    bitmap
                )
            );

        IMFMediaBuffer* buffer = nullptr;
        IMFSample* sample = nullptr;

        const long long totalFrames =
            static_cast<long long>(
                std::ceil(
                    duration * FPS
                )
            );

        for (
            long long frame = 0;
            frame < totalFrames;
            ++frame
        )
        {
            currentTime =
                static_cast<double>(frame) /
                static_cast<double>(FPS);

            ResetPlaybackState();

            ProcessEventsTo(
                currentTime
            );

            RECT scene{
                0,
                0,
                VIDEO_WIDTH,
                VIDEO_HEIGHT
            };

            RenderScene(
                videoDC,
                VIDEO_WIDTH,
                VIDEO_HEIGHT
            );

            hr =
                MFCreateMemoryBuffer(
                    VIDEO_WIDTH *
                    VIDEO_HEIGHT *
                    4,
                    &buffer
                );

            if (FAILED(hr))
                break;

            BYTE* destination = nullptr;
            DWORD maxLength = 0;
            DWORD currentLength = 0;

            hr =
                buffer->Lock(
                    &destination,
                    &maxLength,
                    &currentLength
                );

            if (FAILED(hr))
            {
                buffer->Release();
                buffer = nullptr;
                break;
            }

            const size_t byteCount =
                static_cast<size_t>(
                    VIDEO_WIDTH
                ) *
                static_cast<size_t>(
                    VIDEO_HEIGHT
                ) *
                4;

            memcpy(
                destination,
                bits,
                byteCount
            );

            buffer->Unlock();

            hr =
                buffer->SetCurrentLength(
                    static_cast<DWORD>(
                        byteCount
                    )
                );

            if (FAILED(hr))
            {
                buffer->Release();
                buffer = nullptr;
                break;
            }

            hr =
                MFCreateSample(
                    &sample
                );

            if (FAILED(hr))
            {
                buffer->Release();
                buffer = nullptr;
                break;
            }

            hr =
                sample->AddBuffer(
                    buffer
                );

            if (FAILED(hr))
            {
                sample->Release();
                sample = nullptr;

                buffer->Release();
                buffer = nullptr;

                break;
            }

            const LONGLONG sampleTime =
                static_cast<LONGLONG>(
                    frame
                ) *
                10000000LL /
                FPS;

            sample->SetSampleTime(
                sampleTime
            );

            sample->SetSampleDuration(
                10000000LL / FPS
            );

            hr =
                writer->WriteSample(
                    streamIndex,
                    sample
                );

            sample->Release();
            sample = nullptr;

            buffer->Release();
            buffer = nullptr;

            if (FAILED(hr))
                break;
        }

        SelectObject(
            videoDC,
            oldBitmap
        );

        DeleteObject(bitmap);
        DeleteDC(videoDC);

        if (FAILED(hr))
            break;

        hr =
            writer->Finalize();

        if (FAILED(hr))
            break;

        success = true;

    } while (false);

    if (writer)
        writer->Release();

    if (inputType)
        inputType->Release();

    if (outputType)
        outputType->Release();

    if (attributes)
        attributes->Release();

    MFShutdown();

    currentTime = 0.0;
    ResetPlaybackState();

    if (success)
    {
        MessageBoxW(
            hwndMain,
            (L"MP4 exported successfully:\n\n" +
             outputPath).c_str(),
            L"Export Complete",
            MB_OK |
            MB_ICONINFORMATION
        );
    }
    else
    {
        MessageBoxW(
            hwndMain,
            L"MP4 export failed.\n\n"
            L"Windows Media Foundation could not "
            L"create the H.264 video.",
            L"Export Failed",
            MB_OK |
            MB_ICONERROR
        );
    }

    InvalidateRect(
        hwndMain,
        nullptr,
        TRUE
    );

    return success;
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
        case WM_CREATE:
        {
            hwndTimeline =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD |
                    WS_VISIBLE |
                    ES_MULTILINE |
                    ES_AUTOVSCROLL |
                    ES_WANTRETURN |
                    WS_VSCROLL |
                    WS_HSCROLL,
                    20,
                    20,
                    1050,
                    220,
                    hwnd,
                    nullptr,
                    nullptr,
                    nullptr
                );

            SendMessageW(
                hwndTimeline,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(
                    GetStockObject(
                        DEFAULT_GUI_FONT
                    )
                ),
                TRUE
            );

            hwndLoad =
                CreateWindowW(
                    L"BUTTON",
                    L"Load Timeline",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    20,
                    255,
                    150,
                    40,
                    hwnd,
                    reinterpret_cast<HMENU>(1001),
                    nullptr,
                    nullptr
                );

            hwndPlay =
                CreateWindowW(
                    L"BUTTON",
                    L"Play",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    180,
                    255,
                    100,
                    40,
                    hwnd,
                    reinterpret_cast<HMENU>(1002),
                    nullptr,
                    nullptr
                );

            hwndReset =
                CreateWindowW(
                    L"BUTTON",
                    L"Reset",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    290,
                    255,
                    100,
                    40,
                    hwnd,
                    reinterpret_cast<HMENU>(1003),
                    nullptr,
                    nullptr
                );

            hwndExport =
                CreateWindowW(
                    L"BUTTON",
                    L"Export MP4",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    400,
                    255,
                    130,
                    40,
                    hwnd,
                    reinterpret_cast<HMENU>(1004),
                    nullptr,
                    nullptr
                );

            QueryPerformanceFrequency(
                &performanceFrequency
            );

            QueryPerformanceCounter(
                &lastPerformanceTime
            );

            SetTimer(
                hwnd,
                TIMER_PREVIEW,
                16,
                nullptr
            );

            return 0;
        }

        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case 1001:
                    LoadTimelineFromEditor();
                    return 0;

                case 1002:
                {
                    if (events.empty())
                        return 0;

                    playing = !playing;

                    SetWindowTextW(
                        hwndPlay,
                        playing
                            ? L"Pause"
                            : L"Play"
                    );

                    QueryPerformanceCounter(
                        &lastPerformanceTime
                    );

                    return 0;
                }

                case 1003:
                {
                    playing = false;
                    currentTime = 0.0;

                    ResetPlaybackState();

                    SetWindowTextW(
                        hwndPlay,
                        L"Play"
                    );

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        TRUE
                    );

                    return 0;
                }

                case 1004:
                    ExportMP4();
                    return 0;
            }

            break;
        }

        case WM_TIMER:
        {
            if (wParam == TIMER_PREVIEW &&
                playing)
            {
                LARGE_INTEGER now{};

                QueryPerformanceCounter(
                    &now
                );

                double delta =
                    static_cast<double>(
                        now.QuadPart -
                        lastPerformanceTime.QuadPart
                    ) /
                    static_cast<double>(
                        performanceFrequency.QuadPart
                    );

                lastPerformanceTime =
                    now;

                currentTime += delta;

                if (currentTime >= duration)
                {
                    currentTime = duration;
                    playing = false;

                    SetWindowTextW(
                        hwndPlay,
                        L"Play"
                    );

                    ResetPlaybackState();
                }

                InvalidateRect(
                    hwnd,
                    nullptr,
                    TRUE
                );
            }

            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            HDC hdc =
                BeginPaint(
                    hwnd,
                    &ps
                );

            RECT client{};
            GetClientRect(
                hwnd,
                &client
            );

            const int sceneTop = 315;

            FillRectColor(
                hdc,
                0,
                sceneTop,
                client.right,
                client.bottom,
                RGB(18, 18, 22)
            );

            // Preview uses scaled scene.
            int previewWidth =
                client.right;

            int previewHeight =
                client.bottom - sceneTop;

            if (previewWidth > 0 &&
                previewHeight > 0)
            {
                HDC memDC =
                    CreateCompatibleDC(hdc);

                BITMAPINFO bmi{};

                bmi.bmiHeader.biSize =
                    sizeof(BITMAPINFOHEADER);

                bmi.bmiHeader.biWidth =
                    previewWidth;

                bmi.bmiHeader.biHeight =
                    -previewHeight;

                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression =
                    BI_RGB;

                void* bits = nullptr;

                HBITMAP bitmap =
                    CreateDIBSection(
                        hdc,
                        &bmi,
                        DIB_RGB_COLORS,
                        &bits,
                        nullptr,
                        0
                    );

                if (bitmap)
                {
                    HBITMAP oldBitmap =
                        static_cast<HBITMAP>(
                            SelectObject(
                                memDC,
                                bitmap
                            )
                        );

                    // Render directly at preview resolution.
                    ResetPlaybackState();
                    ProcessEventsTo(
                        currentTime
                    );

                    RenderScene(
                        memDC,
                        previewWidth,
                        previewHeight
                    );

                    BitBlt(
                        hdc,
                        0,
                        sceneTop,
                        previewWidth,
                        previewHeight,
                        memDC,
                        0,
                        0,
                        SRCCOPY
                    );

                    SelectObject(
                        memDC,
                        oldBitmap
                    );

                    DeleteObject(bitmap);
                }

                DeleteDC(memDC);
            }

            EndPaint(
                hwnd,
                &ps
            );

            return 0;
        }

        case WM_SIZE:
        {
            int width =
                LOWORD(lParam);

            if (hwndTimeline)
            {
                MoveWindow(
                    hwndTimeline,
                    20,
                    20,
                    width - 40,
                    220,
                    TRUE
                );
            }

            return 0;
        }

        case WM_DESTROY:
        {
            KillTimer(
                hwnd,
                TIMER_PREVIEW
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
    int nCmdShow
)
{
    WNDCLASSW wc{};

    wc.lpfnWndProc =
        WindowProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        L"APMTimelineVisualizer";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        static_cast<HBRUSH>(
            GetStockObject(
                WHITE_BRUSH
            )
        );

    if (!RegisterClassW(&wc))
        return 1;

    hwndMain =
        CreateWindowExW(
            0,
            wc.lpszClassName,
            L"APM Timeline Visualizer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            DEFAULT_WIDTH,
            DEFAULT_HEIGHT,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!hwndMain)
        return 1;

    ShowWindow(
        hwndMain,
        nCmdShow
    );

    UpdateWindow(hwndMain);

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

    return static_cast<int>(
        msg.wParam
    );
}
