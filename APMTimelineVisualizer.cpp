#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")


// ============================================================
// SETTINGS
// ============================================================

static const int VIDEO_WIDTH  = 1920;
static const int VIDEO_HEIGHT = 1080;
static const int VIDEO_FPS    = 60;

static const double FLASH_TIME = 0.12;

static const wchar_t* DEFAULT_INPUT =
    L"Inputs.txt";

static const wchar_t* DEFAULT_OUTPUT =
    L"APM_Replay.mp4";


// ============================================================
// INPUT EVENT
// ============================================================

struct InputEvent
{
    double time;
    std::wstring name;
    bool release;
};


// ============================================================
// GLOBALS
// ============================================================

static HWND hwnd = nullptr;

static std::vector<InputEvent> events;

static std::vector<double> actionTimes;

static size_t nextEvent = 0;

static double currentTime = 0.0;

static double firstTimestamp = 0.0;

static double duration = 0.0;

static bool playing = false;

static bool exporting = false;

static HANDLE exportThread = nullptr;

static std::wstring inputPath =
    DEFAULT_INPUT;

static std::wstring outputPath =
    DEFAULT_OUTPUT;

static LARGE_INTEGER performanceFrequency = {};
static LARGE_INTEGER performanceStart = {};


// ============================================================
// ACTIVE BUTTON STATE
// ============================================================

struct ButtonState
{
    std::wstring name;
    bool down;
    double lastPress;
};

static std::vector<ButtonState> buttonStates;


// ============================================================
// TIMELINE PARSER
// ============================================================

double ParseClockTime(
    int hour,
    int minute,
    double second,
    bool pm
)
{
    if (hour == 12)
        hour = 0;

    if (pm)
        hour += 12;

    return
        hour * 3600.0 +
        minute * 60.0 +
        second;
}


bool ParseTimelineLine(
    const std::wstring& line,
    InputEvent& result
)
{
    if (line.empty())
        return false;

    if (line[0] == L'#')
        return false;

    // Skip the count section.
    if (
        line.find(L"===") != std::wstring::npos
    )
        return false;

    std::wstringstream ss(line);

    int hour = 0;
    int minute = 0;

    std::wstring secondText;
    std::wstring ampm;
    std::wstring name;

    if (
        !(ss >> hour >> minute >> secondText >> ampm >> name)
    )
    {
        return false;
    }

    if (
        hour < 0 ||
        hour > 23 ||
        minute < 0 ||
        minute > 59
    )
    {
        return false;
    }

    double second =
        _wtof(
            secondText.c_str()
        );

    if (
        second < 0.0 ||
        second >= 60.0
    )
    {
        return false;
    }

    bool pm = false;

    if (
        ampm == L"PM" ||
        ampm == L"pm" ||
        ampm == L"Pm" ||
        ampm == L"pM"
    )
    {
        pm = true;
    }

    double timestamp =
        ParseClockTime(
            hour,
            minute,
            second,
            pm
        );

    result.time = timestamp;

    result.release = false;

    if (
        name.size() >= 3 &&
        name.substr(
            name.size() - 3
        ) == L"_UP"
    )
    {
        result.release = true;

        name.resize(
            name.size() - 3
        );
    }

    result.name = name;

    return !name.empty();
}


// ============================================================
// LOAD TIMELINE
// ============================================================

bool LoadTimeline(
    const std::wstring& path
)
{
    std::wifstream file(
        path
    );

    if (!file.is_open())
        return false;

    events.clear();
    actionTimes.clear();
    buttonStates.clear();

    std::wstring line;

    while (
        std::getline(
            file,
            line
        )
    )
    {
        InputEvent event;

        if (
            ParseTimelineLine(
                line,
                event
            )
        )
        {
            events.push_back(
                event
            );
        }
    }

    if (events.empty())
        return false;

    std::sort(
        events.begin(),
        events.end(),
        [](
            const InputEvent& a,
            const InputEvent& b
        )
        {
            return a.time < b.time;
        }
    );

    firstTimestamp =
        events.front().time;

    for (auto& event : events)
    {
        event.time -=
            firstTimestamp;

        if (event.time < 0.0)
            event.time = 0.0;

        if (!event.release)
        {
            actionTimes.push_back(
                event.time
            );
        }
    }

    duration =
        events.back().time;

    if (duration < 1.0)
        duration = 1.0;

    nextEvent = 0;

    currentTime = 0.0;

    playing = false;

    return true;
}


// ============================================================
// BUTTON STATE
// ============================================================

ButtonState* FindButton(
    const std::wstring& name
)
{
    for (auto& button : buttonStates)
    {
        if (button.name == name)
            return &button;
    }

    return nullptr;
}


void SetButtonDown(
    const std::wstring& name,
    bool down,
    double time
)
{
    ButtonState* button =
        FindButton(
            name
        );

    if (!button)
    {
        ButtonState newButton;

        newButton.name =
            name;

        newButton.down =
            down;

        newButton.lastPress =
            time;

        buttonStates.push_back(
            newButton
        );

        return;
    }

    button->down =
        down;

    if (down)
        button->lastPress =
            time;
}


// ============================================================
// RESET REPLAY STATE
// ============================================================

void ResetReplay()
{
    nextEvent = 0;

    currentTime = 0.0;

    buttonStates.clear();
}


// ============================================================
// PROCESS EVENTS
// ============================================================

void ProcessEventsUntil(
    double time
)
{
    while (
        nextEvent < events.size() &&
        events[nextEvent].time <= time
    )
    {
        const InputEvent& event =
            events[nextEvent];

        SetButtonDown(
            event.name,
            !event.release,
            event.time
        );

        nextEvent++;
    }
}


// ============================================================
// ROLLING APM
// ============================================================

int GetAPMAtTime(
    double time
)
{
    const double cutoff =
        time - 60.0;

    int count = 0;

    for (
        auto it = actionTimes.rbegin();
        it != actionTimes.rend();
        ++it
    )
    {
        if (*it > time)
            continue;

        if (*it < cutoff)
            break;

        count++;
    }

    return count;
}


// ============================================================
// BUTTON HELPER
// ============================================================

bool IsDown(
    const std::wstring& name
)
{
    ButtonState* button =
        FindButton(
            name
        );

    if (!button)
        return false;

    return button->down;
}


double GetFlash(
    const std::wstring& name,
    double time
)
{
    ButtonState* button =
        FindButton(
            name
        );

    if (!button)
        return 0.0;

    if (button->down)
        return 1.0;

    double age =
        time -
        button->lastPress;

    if (
        age < 0.0 ||
        age >= FLASH_TIME
    )
    {
        return 0.0;
    }

    return
        1.0 -
        age / FLASH_TIME;
}


// ============================================================
// DRAW HELPERS
// ============================================================

void FillRoundRect(
    HDC dc,
    int x,
    int y,
    int w,
    int h,
    int radius,
    HBRUSH brush
)
{
    HPEN pen =
        CreatePen(
            PS_SOLID,
            1,
            RGB(20, 20, 20)
        );

    HGDIOBJ oldPen =
        SelectObject(
            dc,
            pen
        );

    HGDIOBJ oldBrush =
        SelectObject(
            dc,
            brush
        );

    RoundRect(
        dc,
        x,
        y,
        x + w,
        y + h,
        radius,
        radius
    );

    SelectObject(
        dc,
        oldPen
    );

    SelectObject(
        dc,
        oldBrush
    );

    DeleteObject(
        pen
    );
}


void DrawTextCentered(
    HDC dc,
    const std::wstring& text,
    RECT rect,
    HFONT font,
    COLORREF color
)
{
    HGDIOBJ old =
        SelectObject(
            dc,
            font
        );

    SetBkMode(
        dc,
        TRANSPARENT
    );

    SetTextColor(
        dc,
        color
    );

    DrawTextW(
        dc,
        text.c_str(),
        -1,
        &rect,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(
        dc,
        old
    );
}


// ============================================================
// KEY DRAWING
// ============================================================

void DrawKey(
    HDC dc,
    const std::wstring& name,
    int x,
    int y,
    int w,
    int h,
    double time,
    HFONT font
)
{
    bool down =
        IsDown(name);

    double flash =
        GetFlash(
            name,
            time
        );

    COLORREF normal =
        RGB(
            38,
            38,
            42
        );

    COLORREF pressed =
        RGB(
            235,
            165,
            55
        );

    COLORREF border =
        RGB(
            100,
            100,
            105
        );

    COLORREF text =
        RGB(
            245,
            245,
            245
        );

    if (down || flash > 0.0)
    {
        double amount =
            down ? 1.0 : flash;

        int r =
            static_cast<int>(
                38 +
                (235 - 38) *
                amount
            );

        int g =
            static_cast<int>(
                38 +
                (165 - 38) *
                amount
            );

        int b =
            static_cast<int>(
                42 +
                (55 - 42) *
                amount
            );

        pressed =
            RGB(
                r,
                g,
                b
            );

        normal =
            pressed;

        border =
            RGB(
                255,
                210,
                100
            );
    }

    HBRUSH brush =
        CreateSolidBrush(
            normal
        );

    FillRoundRect(
        dc,
        x,
        y,
        w,
        h,
        8,
        brush
    );

    DeleteObject(
        brush
    );

    RECT textRect = {
        x,
        y,
        x + w,
        y + h
    };

    DrawTextCentered(
        dc,
        name,
        textRect,
        font,
        text
    );
}


// ============================================================
// DRAW KEYBOARD
// ============================================================

void DrawKeyboard(
    HDC dc,
    int originX,
    int originY,
    double time
)
{
    HFONT keyFont =
        CreateFontW(
            -24,
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

    const int keyW = 72;
    const int keyH = 62;
    const int gap = 8;

    // Number row
    const wchar_t* row1[] =
    {
        L"ESC",
        L"1",
        L"2",
        L"3",
        L"4",
        L"5",
        L"6",
        L"7",
        L"8",
        L"9",
        L"0"
    };

    for (int i = 0; i < 11; ++i)
    {
        DrawKey(
            dc,
            row1[i],
            originX +
                i * (keyW + gap),
            originY,
            keyW,
            keyH,
            time,
            keyFont
        );
    }

    // Q row
    const wchar_t* row2[] =
    {
        L"TAB",
        L"Q",
        L"W",
        L"E",
        L"R",
        L"T",
        L"Y",
        L"U",
        L"I",
        L"O",
        L"P"
    };

    for (int i = 0; i < 11; ++i)
    {
        int extra =
            i == 0 ? 30 : 0;

        DrawKey(
            dc,
            row2[i],
            originX +
                extra +
                i * (keyW + gap),
            originY +
                keyH +
                gap,
            keyW,
            keyH,
            time,
            keyFont
        );
    }

    // A row
    const wchar_t* row3[] =
    {
        L"CAPS",
        L"A",
        L"S",
        L"D",
        L"F",
        L"G",
        L"H",
        L"J",
        L"K",
        L"L"
    };

    for (int i = 0; i < 10; ++i)
    {
        int extra =
            i == 0 ? 55 : 0;

        int width =
            i == 0 ? 90 : keyW;

        DrawKey(
            dc,
            row3[i],
            originX +
                extra +
                i * (keyW + gap),
            originY +
                2 * (keyH + gap),
            width,
            keyH,
            time,
            keyFont
        );
    }

    // Z row
    const wchar_t* row4[] =
    {
        L"SHIFT",
        L"Z",
        L"X",
        L"C",
        L"V",
        L"B",
        L"N",
        L"M"
    };

    for (int i = 0; i < 8; ++i)
    {
        int width =
            i == 0 ? 115 : keyW;

        int extra =
            i == 0 ? 0 : 43;

        DrawKey(
            dc,
            row4[i],
            originX +
                extra +
                i * (keyW + gap),
            originY +
                3 * (keyH + gap),
            width,
            keyH,
            time,
            keyFont
        );
    }

    // Space
    DrawKey(
        dc,
        L"SPACE",
        originX + 210,
        originY +
            4 * (keyH + gap),
        390,
        keyH,
        time,
        keyFont
    );

    DeleteObject(
        keyFont
    );
}


// ============================================================
// DRAW MOUSE
// ============================================================

void DrawMouse(
    HDC dc,
    int x,
    int y,
    double time
)
{
    HFONT font =
        CreateFontW(
            -24,
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

    // Mouse body
    HBRUSH body =
        CreateSolidBrush(
            RGB(
                38,
                38,
                42
            )
        );

    HPEN pen =
        CreatePen(
            PS_SOLID,
            4,
            RGB(
                100,
                100,
                105
            )
        );

    HGDIOBJ oldPen =
        SelectObject(
            dc,
            pen
        );

    HGDIOBJ oldBrush =
        SelectObject(
            dc,
            body
        );

    RoundRect(
        dc,
        x,
        y,
        x + 190,
        y + 300,
        45,
        45
    );

    SelectObject(
        dc,
        oldBrush
    );

    SelectObject(
        dc,
        oldPen
    );

    DeleteObject(
        body
    );

    DeleteObject(
        pen
    );

    // Middle divider
    HPEN divider =
        CreatePen(
            PS_SOLID,
            3,
            RGB(
                90,
                90,
                95
            )
        );

    oldPen =
        SelectObject(
            dc,
            divider
        );

    MoveToEx(
        dc,
        x + 95,
        y + 10,
        nullptr
    );

    LineTo(
        dc,
        x + 95,
        y + 115
    );

    SelectObject(
        dc,
        oldPen
    );

    DeleteObject(
        divider
    );

    // LMB
    bool lmb =
        IsDown(L"LMB");

    HBRUSH leftBrush =
        CreateSolidBrush(
            lmb
                ? RGB(235, 165, 55)
                : RGB(55, 55, 60)
        );

    RECT leftRect = {
        x + 12,
        y + 12,
        x + 91,
        y + 105
    };

    FillRect(
        dc,
        &leftRect,
        leftBrush
    );

    DeleteObject(
        leftBrush
    );

    // RMB
    bool rmb =
        IsDown(L"RMB");

    HBRUSH rightBrush =
        CreateSolidBrush(
            rmb
                ? RGB(235, 165, 55)
                : RGB(55, 55, 60)
        );

    RECT rightRect = {
        x + 99,
        y + 12,
        x + 178,
        y + 105
    };

    FillRect(
        dc,
        &rightRect,
        rightBrush
    );

    DeleteObject(
        rightBrush
    );

    // Wheel
    HBRUSH wheelBrush =
        CreateSolidBrush(
            RGB(
                100,
                100,
                105
            )
        );

    RECT wheel = {
        x + 82,
        y + 120,
        x + 108,
        y + 175
    };

    FillRect(
        dc,
        &wheel,
        wheelBrush
    );

    DeleteObject(
        wheelBrush
    );

    RECT lmbText = {
        x + 10,
        y + 185,
        x + 90,
        y + 225
    };

    RECT rmbText = {
        x + 100,
        y + 185,
        x + 180,
        y + 225
    };

    DrawTextCentered(
        dc,
        L"LMB",
        lmbText,
        font,
        RGB(245,245,245)
    );

    DrawTextCentered(
        dc,
        L"RMB",
        rmbText,
        font,
        RGB(245,245,245)
    );

    DeleteObject(
        font
    );
}


// ============================================================
// DRAW WHOLE FRAME
// ============================================================

void DrawFrame(
    HDC dc,
    int width,
    int height,
    double time
)
{
    // Background
    HBRUSH background =
        CreateSolidBrush(
            RGB(
                16,
                16,
                18
            )
        );

    RECT full = {
        0,
        0,
        width,
        height
    };

    FillRect(
        dc,
        &full,
        background
    );

    DeleteObject(
        background
    );

    // APM
    int apm =
        GetAPMAtTime(
            time
        );

    HFONT apmFont =
        CreateFontW(
            -72,
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

    std::wstring apmText =
        L"APM " +
        std::to_wstring(
            apm
        );

    RECT apmRect = {
        0,
        45,
        width,
        135
    };

    DrawTextCentered(
        dc,
        apmText,
        apmRect,
        apmFont,
        RGB(
            255,
            255,
            255
        )
    );

    DeleteObject(
        apmFont
    );

    // Timeline position
    int totalSeconds =
        static_cast<int>(
            time
        );

    int minutes =
        totalSeconds / 60;

    int seconds =
        totalSeconds % 60;

    int millis =
        static_cast<int>(
            (time -
             totalSeconds) *
            1000.0
        );

    wchar_t timeBuffer[64];

    swprintf_s(
        timeBuffer,
        L"%02d:%02d.%03d",
        minutes,
        seconds,
        millis
    );

    HFONT timeFont =
        CreateFontW(
            -30,
            0,
            0,
            0,
            FW_NORMAL,
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

    RECT timeRect = {
        0,
        130,
        width,
        175
    };

    DrawTextCentered(
        dc,
        timeBuffer,
        timeRect,
        timeFont,
        RGB(
            170,
            170,
            175
        )
    );

    DeleteObject(
        timeFont
    );

    // Keyboard
    DrawKeyboard(
        dc,
        170,
        245,
        time
    );

    // Mouse
    DrawMouse(
        dc,
        1450,
        380,
        time
    );

    // Progress bar
    int barX = 170;
    int barY = 930;
    int barW = 1580;
    int barH = 12;

    HBRUSH barBackground =
        CreateSolidBrush(
            RGB(
                45,
                45,
                50
            )
        );

    RECT bar = {
        barX,
        barY,
        barX + barW,
        barY + barH
    };

    FillRect(
        dc,
        &bar,
        barBackground
    );

    DeleteObject(
        barBackground
    );

    double progress =
        duration > 0.0
            ? time / duration
            : 0.0;

    if (progress < 0.0)
        progress = 0.0;

    if (progress > 1.0)
        progress = 1.0;

    HBRUSH progressBrush =
        CreateSolidBrush(
            RGB(
                235,
                165,
                55
            )
        );

    RECT progressRect = {
        barX,
        barY,
        barX +
            static_cast<int>(
                barW *
                progress
            ),
        barY + barH
    };

    FillRect(
        dc,
        &progressRect,
        progressBrush
    );

    DeleteObject(
        progressBrush
    );
}


// ============================================================
// CREATE RGB32 BUFFER
// ============================================================

HBITMAP CreateFrameBitmap(
    HDC referenceDC,
    void** bits
)
{
    BITMAPINFO bmi = {};

    bmi.bmiHeader.biSize =
        sizeof(
            BITMAPINFOHEADER
        );

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

    return CreateDIBSection(
        referenceDC,
        &bmi,
        DIB_RGB_COLORS,
        bits,
        nullptr,
        0
    );
}


// ============================================================
// MP4 EXPORT
// ============================================================

bool ExportMP4()
{
    if (events.empty())
        return false;

    exporting = true;

    HRESULT hr =
        MFStartup(
            MF_VERSION
        );

    if (FAILED(hr))
    {
        exporting = false;
        return false;
    }

    IMFAttributes* attributes =
        nullptr;

    IMFSinkWriter* writer =
        nullptr;

    IMFMediaType* outputType =
        nullptr;

    IMFMediaType* inputType =
        nullptr;

    hr =
        MFCreateAttributes(
            &attributes,
            1
        );

    if (FAILED(hr))
        goto cleanup;

    hr =
        MFCreateSinkWriterFromURL(
            outputPath.c_str(),
            nullptr,
            attributes,
            &writer
        );

    if (FAILED(hr))
        goto cleanup;

    // --------------------------------------------------------
    // OUTPUT H264
    // --------------------------------------------------------

    hr =
        MFCreateMediaType(
            &outputType
        );

    if (FAILED(hr))
        goto cleanup;

    outputType->SetGUID(
        MF_MT_MAJOR_TYPE,
        MFMediaType_Video
    );

    outputType->SetGUID(
        MF_MT_SUBTYPE,
        MFVideoFormat_H264
    );

    outputType->SetUINT32(
        MF_MT_AVG_BITRATE,
        12000000
    );

    outputType->SetUINT32(
        MF_MT_INTERLACE_MODE,
        MFVideoInterlace_Progressive
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
        VIDEO_FPS,
        1
    );

    MFSetAttributeRatio(
        outputType,
        MF_MT_PIXEL_ASPECT_RATIO,
        1,
        1
    );

    DWORD streamIndex = 0;

    hr =
        writer->AddStream(
            outputType,
            &streamIndex
        );

    if (FAILED(hr))
        goto cleanup;

    // --------------------------------------------------------
    // INPUT RGB32
    // --------------------------------------------------------

    hr =
        MFCreateMediaType(
            &inputType
        );

    if (FAILED(hr))
        goto cleanup;

    inputType->SetGUID(
        MF_MT_MAJOR_TYPE,
        MFMediaType_Video
    );

    inputType->SetGUID(
        MF_MT_SUBTYPE,
        MFVideoFormat_RGB32
    );

    inputType->SetUINT32(
        MF_MT_INTERLACE_MODE,
        MFVideoInterlace_Progressive
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
        VIDEO_FPS,
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
        goto cleanup;

    hr =
        writer->BeginWriting();

    if (FAILED(hr))
        goto cleanup;

    // --------------------------------------------------------
    // CREATE FRAME DC
    // --------------------------------------------------------

    HDC screenDC =
        GetDC(nullptr);

    HDC frameDC =
        CreateCompatibleDC(
            screenDC
        );

    void* bits = nullptr;

    HBITMAP bitmap =
        CreateFrameBitmap(
            screenDC,
            &bits
        );

    if (!bitmap)
    {
        DeleteDC(frameDC);
        ReleaseDC(nullptr, screenDC);
        hr = E_FAIL;
        goto cleanup;
    }

    HBITMAP oldBitmap =
        static_cast<HBITMAP>(
            SelectObject(
                frameDC,
                bitmap
            )
        );

    // --------------------------------------------------------
    // EXPORT FRAMES
    // --------------------------------------------------------

    const long long totalFrames =
        static_cast<long long>(
            std::ceil(
                duration *
                VIDEO_FPS
            )
        ) + 1;

    for (
        long long frame = 0;
        frame < totalFrames;
        ++frame
    )
    {
        double frameTime =
            static_cast<double>(
                frame
            ) /
            VIDEO_FPS;

        ResetReplay();

        ProcessEventsUntil(
            frameTime
        );

        DrawFrame(
            frameDC,
            VIDEO_WIDTH,
            VIDEO_HEIGHT,
            frameTime
        );

        IMFSample* sample =
            nullptr;

        IMFMediaBuffer* buffer =
            nullptr;

        DWORD bufferLength =
            VIDEO_WIDTH *
            VIDEO_HEIGHT *
            4;

        hr =
            MFCreateMemoryBuffer(
                bufferLength,
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
            break;
        }

        memcpy(
            destination,
            bits,
            bufferLength
        );

        buffer->Unlock();

        buffer->SetCurrentLength(
            bufferLength
        );

        hr =
            MFCreateSample(
                &sample
            );

        if (FAILED(hr))
        {
            buffer->Release();
            break;
        }

        sample->AddBuffer(
            buffer
        );

        sample->SetSampleTime(
            frame *
            10000000LL /
            VIDEO_FPS
        );

        sample->SetSampleDuration(
            10000000LL /
            VIDEO_FPS
        );

        hr =
            writer->WriteSample(
                streamIndex,
                sample
            );

        sample->Release();

        buffer->Release();

        if (FAILED(hr))
            break;

        // Update preview/export progress.
        if (
            hwnd &&
            frame % 30 == 0
        )
        {
            wchar_t title[256];

            double percent =
                totalFrames > 0
                    ? (
                        static_cast<double>(
                            frame
                        ) /
                        totalFrames
                    ) * 100.0
                    : 0.0;

            swprintf_s(
                title,
                L"APM Timeline Visualizer - Exporting %.0f%%",
                percent
            );

            SetWindowTextW(
                hwnd,
                title
            );
        }
    }

    SelectObject(
        frameDC,
        oldBitmap
    );

    DeleteObject(
        bitmap
    );

    DeleteDC(
        frameDC
    );

    ReleaseDC(
        nullptr,
        screenDC
    );

    writer->Finalize();

cleanup:

    if (inputType)
        inputType->Release();

    if (outputType)
        outputType->Release();

    if (writer)
        writer->Release();

    if (attributes)
        attributes->Release();

    MFShutdown();

    exporting = false;

    if (hwnd)
    {
        SetWindowTextW(
            hwnd,
            L"APM Timeline Visualizer"
        );
    }

    return SUCCEEDED(hr);
}


// ============================================================
// EXPORT THREAD
// ============================================================

DWORD WINAPI ExportThreadProc(
    LPVOID
)
{
    ExportMP4();

    return 0;
}


// ============================================================
// OPEN FILE
// ============================================================

bool OpenTimeline()
{
    wchar_t path[MAX_PATH] = {};

    OPENFILENAMEW dialog = {};

    dialog.lStructSize =
        sizeof(dialog);

    dialog.hwndOwner =
        hwnd;

    dialog.lpstrFilter =
        L"Timeline files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";

    dialog.lpstrFile =
        path;

    dialog.nMaxFile =
        MAX_PATH;

    dialog.Flags =
        OFN_FILEMUSTEXIST |
        OFN_PATHMUSTEXIST;

    if (
        !GetOpenFileNameW(
            &dialog
        )
    )
    {
        return false;
    }

    if (
        !LoadTimeline(
            path
        )
    )
    {
        MessageBoxW(
            hwnd,
            L"Could not read the timeline.",
            L"Timeline Error",
            MB_ICONERROR
        );

        return false;
    }

    inputPath =
        path;

    ResetReplay();

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE
    );

    return true;
}


// ============================================================
// SAVE OUTPUT FILE
// ============================================================

bool ChooseOutput()
{
    wchar_t path[MAX_PATH] = {};

    wcscpy_s(
        path,
        DEFAULT_OUTPUT
    );

    OPENFILENAMEW dialog = {};

    dialog.lStructSize =
        sizeof(dialog);

    dialog.hwndOwner =
        hwnd;

    dialog.lpstrFilter =
        L"MP4 Video (*.mp4)\0*.mp4\0";

    dialog.lpstrFile =
        path;

    dialog.nMaxFile =
        MAX_PATH;

    dialog.lpstrDefExt =
        L"mp4";

    dialog.Flags =
        OFN_OVERWRITEPROMPT |
        OFN_PATHMUSTEXIST;

    if (
        !GetSaveFileNameW(
            &dialog
        )
    )
    {
        return false;
    }

    outputPath =
        path;

    return true;
}


// ============================================================
// WINDOW PROCEDURE
// ============================================================

#define ID_OPEN       1001
#define ID_PLAY       1002
#define ID_EXPORT     1003
#define ID_RESET      1004
#define ID_TIMER      2001

LRESULT CALLBACK WndProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (message)
    {
    case WM_CREATE:
    {
        CreateWindowW(
            L"BUTTON",
            L"Open Timeline",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            20,
            20,
            140,
            35,
            window,
            reinterpret_cast<HMENU>(
                ID_OPEN
            ),
            nullptr,
            nullptr
        );

        CreateWindowW(
            L"BUTTON",
            L"Play",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            170,
            20,
            100,
            35,
            window,
            reinterpret_cast<HMENU>(
                ID_PLAY
            ),
            nullptr,
            nullptr
        );

        CreateWindowW(
            L"BUTTON",
            L"Export MP4",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            280,
            20,
            120,
            35,
            window,
            reinterpret_cast<HMENU>(
                ID_EXPORT
            ),
            nullptr,
            nullptr
        );

        CreateWindowW(
            L"BUTTON",
            L"Reset",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            410,
            20,
            100,
            35,
            window,
            reinterpret_cast<HMENU>(
                ID_RESET
            ),
            nullptr,
            nullptr
        );

        SetTimer(
            window,
            ID_TIMER,
            16,
            nullptr
        );

        return 0;
    }


    case WM_COMMAND:

        switch (LOWORD(wParam))
        {
        case ID_OPEN:

            if (!exporting)
                OpenTimeline();

            return 0;


        case ID_PLAY:

            if (
                !events.empty() &&
                !exporting
            )
            {
                if (
                    currentTime >= duration
                )
                {
                    ResetReplay();
                }

                playing =
                    !playing;
            }

            return 0;


        case ID_EXPORT:

            if (
                !events.empty() &&
                !exporting
            )
            {
                if (
                    !ChooseOutput()
                )
                    return 0;

                if (exportThread)
                {
                    CloseHandle(
                        exportThread
                    );

                    exportThread =
                        nullptr;
                }

                exportThread =
                    CreateThread(
                        nullptr,
                        0,
                        ExportThreadProc,
                        nullptr,
                        0,
                        nullptr
                    );
            }

            return 0;


        case ID_RESET:

            if (!exporting)
            {
                ResetReplay();

                InvalidateRect(
                    window,
                    nullptr,
                    FALSE
                );
            }

            return 0;
        }

        break;


    case WM_TIMER:

        if (
            wParam == ID_TIMER &&
            playing &&
            !exporting
        )
        {
            currentTime +=
                1.0 /
                VIDEO_FPS;

            if (
                currentTime >= duration
            )
            {
                currentTime =
                    duration;

                playing =
                    false;
            }

            ResetReplay();

            ProcessEventsUntil(
                currentTime
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

        RECT client;

        GetClientRect(
            window,
            &client
        );

        HBRUSH background =
            CreateSolidBrush(
                RGB(
                    22,
                    22,
                    25
                )
            );

        FillRect(
            dc,
            &client,
            background
        );

        DeleteObject(
            background
        );

        // Preview area.
        RECT preview = {
            20,
            75,
            client.right - 20,
            client.bottom - 20
        };

        HDC previewDC =
            CreateCompatibleDC(
                dc
            );

        BITMAPINFO bmi = {};

        bmi.bmiHeader.biSize =
            sizeof(
                BITMAPINFOHEADER
            );

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
                dc,
                &bmi,
                DIB_RGB_COLORS,
                &bits,
                nullptr,
                0
            );

        if (bitmap)
        {
            HBITMAP old =
                static_cast<HBITMAP>(
                    SelectObject(
                        previewDC,
                        bitmap
                    )
                );

            ResetReplay();

            ProcessEventsUntil(
                currentTime
            );

            DrawFrame(
                previewDC,
                VIDEO_WIDTH,
                VIDEO_HEIGHT,
                currentTime
            );

            int previewW =
                preview.right -
                preview.left;

            int previewH =
                preview.bottom -
                preview.top;

            double aspect =
                static_cast<double>(
                    VIDEO_WIDTH
                ) /
                VIDEO_HEIGHT;

            if (
                previewW /
                aspect <=
                previewH
            )
            {
                previewH =
                    static_cast<int>(
                        previewW /
                        aspect
                    );
            }
            else
            {
                previewW =
                    static_cast<int>(
                        previewH *
                        aspect
                    );
            }

            int x =
                preview.left +
                (
                    (
                        preview.right -
                        preview.left -
                        previewW
                    ) / 2
                );

            int y =
                preview.top +
                (
                    (
                        preview.bottom -
                        preview.top -
                        previewH
                    ) / 2
                );

            StretchBlt(
                dc,
                x,
                y,
                previewW,
                previewH,
                previewDC,
                0,
                0,
                VIDEO_WIDTH,
                VIDEO_HEIGHT,
                SRCCOPY
            );

            SelectObject(
                previewDC,
                old
            );

            DeleteObject(
                bitmap
            );
        }

        DeleteDC(
            previewDC
        );

        EndPaint(
            window,
            &ps
        );

        return 0;
    }


    case WM_DESTROY:

        KillTimer(
            window,
            ID_TIMER
        );

        if (exportThread)
        {
            WaitForSingleObject(
                exportThread,
                INFINITE
            );

            CloseHandle(
                exportThread
            );

            exportThread =
                nullptr;
        }

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
    int nCmdShow
)
{
    QueryPerformanceFrequency(
        &performanceFrequency
    );

    QueryPerformanceCounter(
        &performanceStart
    );

    LoadTimeline(
        DEFAULT_INPUT
    );

    const wchar_t CLASS_NAME[] =
        L"APMTimelineVisualizerWindow";

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
        static_cast<HBRUSH>(
            GetStockObject(
                BLACK_BRUSH
            )
        );

    RegisterClassW(
        &wc
    );

    hwnd =
        CreateWindowExW(
            0,
            CLASS_NAME,
            L"APM Timeline Visualizer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1320,
            850,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!hwnd)
        return 1;

    ShowWindow(
        hwnd,
        nCmdShow
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

    return 0;
}
