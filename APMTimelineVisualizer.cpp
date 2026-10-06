
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <map>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")


// ============================================================
// VIDEO SETTINGS
// ============================================================

static const int VIDEO_WIDTH  = 1920;
static const int VIDEO_HEIGHT = 1080;
static const int VIDEO_FPS    = 60;

static const double FLASH_TIME = 0.12;
static const double TAIL_TIME  = 0.50;


// ============================================================
// UI IDS
// ============================================================

static const int ID_EDIT =
    1001;

static const int ID_LOAD =
    1002;

static const int ID_PLAY =
    1003;

static const int ID_RESET =
    1004;

static const int ID_EXPORT =
    1005;

static const UINT WM_EXPORT_DONE =
    WM_APP + 50;


// ============================================================
// GLOBALS
// ============================================================

static HWND hwnd = nullptr;
static HWND timelineEdit = nullptr;
static HWND statusText = nullptr;
static HWND playButton = nullptr;

static bool playing = false;
static bool exporting = false;

static HANDLE exportThread = nullptr;

static double currentTime = 0.0;
static double duration = 0.0;

static size_t nextEvent = 0;

static std::wstring outputPath;


// ============================================================
// EVENTS
// ============================================================

struct InputEvent
{
    double time;
    std::wstring name;
    bool release;
};

static std::vector<InputEvent> events;


// ============================================================
// BUTTON STATE
// ============================================================

struct ButtonState
{
    bool down = false;
    double lastPress = -1000.0;
};

static std::map<std::wstring, ButtonState>
    buttonStates;


// ============================================================
// APM ACTION TIMES
// ============================================================

static std::vector<double>
    actionTimes;


// ============================================================
// STRING HELPERS
// ============================================================

std::wstring Trim(
    const std::wstring& text
)
{
    size_t first = 0;

    while (
        first < text.size() &&
        iswspace(text[first])
    )
    {
        first++;
    }

    size_t last =
        text.size();

    while (
        last > first &&
        iswspace(text[last - 1])
    )
    {
        last--;
    }

    return text.substr(
        first,
        last - first
    );
}


bool IsAM(
    const std::wstring& s
)
{
    return
        s == L"AM" ||
        s == L"am" ||
        s == L"Am" ||
        s == L"aM";
}


bool IsPM(
    const std::wstring& s
)
{
    return
        s == L"PM" ||
        s == L"pm" ||
        s == L"Pm" ||
        s == L"pM";
}


// ============================================================
// TIMESTAMP PARSING
// ============================================================

bool ParseColonTimestamp(
    const std::wstring& timestamp,
    const std::wstring& ampm,
    double& result
)
{
    int hour = 0;
    int minute = 0;
    double second = 0.0;

    if (
        swscanf_s(
            timestamp.c_str(),
            L"%d:%d:%lf",
            &hour,
            &minute,
            &second
        ) != 3
    )
    {
        return false;
    }

    if (
        hour < 1 ||
        hour > 12 ||
        minute < 0 ||
        minute > 59 ||
        second < 0.0 ||
        second >= 60.0
    )
    {
        return false;
    }

    if (hour == 12)
        hour = 0;

    if (IsPM(ampm))
        hour += 12;

    result =
        hour * 3600.0 +
        minute * 60.0 +
        second;

    return true;
}


bool ParseSpaceTimestamp(
    const std::wstring& hourText,
    const std::wstring& minuteText,
    const std::wstring& secondText,
    const std::wstring& ampm,
    double& result
)
{
    int hour =
        _wtoi(
            hourText.c_str()
        );

    int minute =
        _wtoi(
            minuteText.c_str()
        );

    double second =
        _wtof(
            secondText.c_str()
        );

    if (
        hour < 1 ||
        hour > 12 ||
        minute < 0 ||
        minute > 59 ||
        second < 0.0 ||
        second >= 60.0
    )
    {
        return false;
    }

    if (hour == 12)
        hour = 0;

    if (IsPM(ampm))
        hour += 12;

    result =
        hour * 3600.0 +
        minute * 60.0 +
        second;

    return true;
}


// ============================================================
// PARSE ONE LINE
// ============================================================

bool ParseLine(
    const std::wstring& source,
    InputEvent& result
)
{
    std::wstring line =
        Trim(source);

    if (line.empty())
        return false;

    if (line[0] == L'#')
        return false;

    if (
        line.find(L"===") == 0
    )
    {
        return false;
    }

    // --------------------------------------------------------
    // FORMAT:
    //
    // 10:46:46.533 PM    1
    // --------------------------------------------------------

    size_t firstSpace =
        line.find_first_of(
            L" \t"
        );

    if (
        firstSpace !=
        std::wstring::npos
    )
    {
        std::wstring timestamp =
            line.substr(
                0,
                firstSpace
            );

        std::wstring remainder =
            Trim(
                line.substr(
                    firstSpace
                )
            );

        size_t secondSpace =
            remainder.find_first_of(
                L" \t"
            );

        if (
            secondSpace !=
            std::wstring::npos
        )
        {
            std::wstring ampm =
                remainder.substr(
                    0,
                    secondSpace
                );

            std::wstring name =
                Trim(
                    remainder.substr(
                        secondSpace
                    )
                );

            double time = 0.0;

            if (
                (IsAM(ampm) ||
                 IsPM(ampm)) &&
                ParseColonTimestamp(
                    timestamp,
                    ampm,
                    time
                )
            )
            {
                if (name.empty())
                    return false;

                bool release = false;

                if (
                    name.size() >= 3 &&
                    name.compare(
                        name.size() - 3,
                        3,
                        L"_UP"
                    ) == 0
                )
                {
                    release = true;

                    name =
                        name.substr(
                            0,
                            name.size() - 3
                        );
                }

                if (name.empty())
                    return false;

                result.time =
                    time;

                result.name =
                    name;

                result.release =
                    release;

                return true;
            }
        }
    }

    // --------------------------------------------------------
    // FORMAT:
    //
    // 10 46 46.533 PM    1
    // --------------------------------------------------------

    std::wstringstream ss(
        line
    );

    std::wstring hour;
    std::wstring minute;
    std::wstring second;
    std::wstring ampm;

    if (
        !(ss >>
          hour >>
          minute >>
          second >>
          ampm)
    )
    {
        return false;
    }

    std::wstring name;

    std::getline(
        ss,
        name
    );

    name =
        Trim(
            name
        );

    if (name.empty())
        return false;

    double time = 0.0;

    if (
        !ParseSpaceTimestamp(
            hour,
            minute,
            second,
            ampm,
            time
        )
    )
    {
        return false;
    }

    bool release = false;

    if (
        name.size() >= 3 &&
        name.compare(
            name.size() - 3,
            3,
            L"_UP"
        ) == 0
    )
    {
        release = true;

        name =
            name.substr(
                0,
                name.size() - 3
            );
    }

    if (name.empty())
        return false;

    result.time =
        time;

    result.name =
        name;

    result.release =
        release;

    return true;
}


// ============================================================
// READ EDIT CONTROL
// ============================================================

std::wstring GetEditText()
{
    int length =
        GetWindowTextLengthW(
            timelineEdit
        );

    if (length <= 0)
        return L"";

    std::wstring text(
        length,
        L'\0'
    );

    GetWindowTextW(
        timelineEdit,
        &text[0],
        length + 1
    );

    return text;
}


// ============================================================
// LOAD TIMELINE
// ============================================================

bool LoadTimeline()
{
    std::wstring text =
        GetEditText();

    if (text.empty())
    {
        SetWindowTextW(
            statusText,
            L"No timeline text was entered."
        );

        return false;
    }

    std::vector<InputEvent>
        parsed;

    std::wstringstream ss(
        text
    );

    std::wstring line;

    while (
        std::getline(
            ss,
            line
        )
    )
    {
        if (
            !line.empty() &&
            line.back() == L'\r'
        )
        {
            line.pop_back();
        }

        InputEvent event;

        if (
            ParseLine(
                line,
                event
            )
        )
        {
            parsed.push_back(
                event
            );
        }
    }

    if (parsed.empty())
    {
        SetWindowTextW(
            statusText,
            L"No valid timeline events found."
        );

        return false;
    }

    // --------------------------------------------------------
    // Convert clock timestamps into elapsed timestamps.
    //
    // Handles midnight crossing.
    // --------------------------------------------------------

    double previous =
        parsed[0].time;

    double dayOffset = 0.0;

    for (
        size_t i = 0;
        i < parsed.size();
        ++i
    )
    {
        double raw =
            parsed[i].time;

        if (
            i > 0 &&
            raw < previous
        )
        {
            dayOffset +=
                24.0 * 60.0 * 60.0;
        }

        parsed[i].time =
            raw +
            dayOffset;

        previous =
            raw;
    }

    double firstTime =
        parsed.front().time;

    for (
        InputEvent& event :
        parsed
    )
    {
        event.time -=
            firstTime;
    }

    std::stable_sort(
        parsed.begin(),
        parsed.end(),
        [](const InputEvent& a,
           const InputEvent& b)
        {
            return a.time < b.time;
        }
    );

    events =
        parsed;

    duration =
        events.back().time;

    buttonStates.clear();

    actionTimes.clear();

    nextEvent = 0;

    currentTime = 0.0;

    playing = false;

    SetWindowTextW(
        playButton,
        L"Play"
    );

    wchar_t status[256];

    swprintf_s(
        status,
        L"Loaded %zu events | Duration %.3f seconds",
        events.size(),
        duration
    );

    SetWindowTextW(
        statusText,
        status
    );

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE
    );

    return true;
}


// ============================================================
// RESET SIMULATION
// ============================================================

void ResetSimulation()
{
    currentTime = 0.0;

    nextEvent = 0;

    buttonStates.clear();

    actionTimes.clear();

    playing = false;

    SetWindowTextW(
        playButton,
        L"Play"
    );

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE
    );
}


// ============================================================
// PROCESS EVENTS
// ============================================================

void ProcessEventsTo(
    double targetTime
)
{
    while (
        nextEvent < events.size() &&
        events[nextEvent].time <= targetTime
    )
    {
        const InputEvent& event =
            events[nextEvent];

        ButtonState& state =
            buttonStates[
                event.name
            ];

        if (event.release)
        {
            state.down = false;
        }
        else
        {
            state.down = true;

            state.lastPress =
                event.time;

            actionTimes.push_back(
                event.time
            );
        }

        nextEvent++;
    }
}


// ============================================================
// APM
// ============================================================

int GetAPMAtTime(
    double time
)
{
    double cutoff =
        time - 60.0;

    int count = 0;

    for (
        auto it =
            actionTimes.rbegin();
        it != actionTimes.rend();
        ++it
    )
    {
        if (*it < cutoff)
            break;

        if (*it <= time)
            count++;
    }

    return count;
}


// ============================================================
// BUTTON ACTIVE STATE
// ============================================================

bool IsButtonPressed(
    const std::wstring& name
)
{
    auto it =
        buttonStates.find(
            name
        );

    if (
        it == buttonStates.end()
    )
    {
        return false;
    }

    const ButtonState& state =
        it->second;

    if (state.down)
        return true;

    return
        currentTime -
        state.lastPress <
        FLASH_TIME;
}


// ============================================================
// DRAW HELPERS
// ============================================================

void FillRectColor(
    HDC hdc,
    int left,
    int top,
    int right,
    int bottom,
    COLORREF color
)
{
    RECT rc{
        left,
        top,
        right,
        bottom
    };

    HBRUSH brush =
        CreateSolidBrush(
            color
        );

    FillRect(
        hdc,
        &rc,
        brush
    );

    DeleteObject(
        brush
    );
}


void DrawTextCentered(
    HDC hdc,
    const std::wstring& text,
    RECT rc,
    int fontSize,
    COLORREF color,
    bool bold = true
)
{
    HFONT font =
        CreateFontW(
            -fontSize,
            0,
            0,
            0,
            bold
                ? FW_BOLD
                : FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            L"Arial"
        );

    HFONT old =
        (HFONT)SelectObject(
            hdc,
            font
        );

    SetBkMode(
        hdc,
        TRANSPARENT
    );

    SetTextColor(
        hdc,
        color
    );

    DrawTextW(
        hdc,
        text.c_str(),
        -1,
        &rc,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(
        hdc,
        old
    );

    DeleteObject(
        font
    );
}


// ============================================================
// DRAW KEY
// ============================================================

void DrawKey(
    HDC hdc,
    int x,
    int y,
    int w,
    int h,
    const std::wstring& name,
    const std::wstring& label
)
{
    bool pressed =
        IsButtonPressed(
            name
        );

    COLORREF fill;

    if (pressed)
    {
        fill =
            RGB(
                230,
                145,
                40
            );
    }
    else
    {
        fill =
            RGB(
                42,
                42,
                48
            );
    }

    FillRectColor(
        hdc,
        x,
        y,
        x + w,
        y + h,
        fill
    );

    HPEN pen =
        CreatePen(
            PS_SOLID,
            2,
            RGB(
                105,
                105,
                115
            )
        );

    HPEN oldPen =
        (HPEN)SelectObject(
            hdc,
            pen
        );

    HBRUSH oldBrush =
        (HBRUSH)SelectObject(
            hdc,
            GetStockObject(
                NULL_BRUSH
            )
        );

    Rectangle(
        hdc,
        x,
        y,
        x + w,
        y + h
    );

    SelectObject(
        hdc,
        oldBrush
    );

    SelectObject(
        hdc,
        oldPen
    );

    DeleteObject(
        pen
    );

    RECT textRc{
        x,
        y,
        x + w,
        y + h
    };

    DrawTextCentered(
        hdc,
        label,
        textRc,
        max(
            16,
            h / 3
        ),
        RGB(
            255,
            255,
            255
        )
    );
}


// ============================================================
// DRAW VIDEO FRAME
// ============================================================

void RenderScene(
    HDC hdc
)
{
    // Background
    FillRectColor(
        hdc,
        0,
        0,
        VIDEO_WIDTH,
        VIDEO_HEIGHT,
        RGB(
            16,
            16,
            20
        )
    );

    // --------------------------------------------------------
    // APM
    // --------------------------------------------------------

    wchar_t apmText[64];

    swprintf_s(
        apmText,
        L"APM %d",
        GetAPMAtTime(
            currentTime
        )
    );

    RECT apmRc{
        0,
        30,
        VIDEO_WIDTH,
        130
    };

    DrawTextCentered(
        hdc,
        apmText,
        apmRc,
        72,
        RGB(
            255,
            255,
            255
        )
    );

    // --------------------------------------------------------
    // Time
    // --------------------------------------------------------

    int minutes =
        static_cast<int>(
            currentTime / 60.0
        );

    double seconds =
        currentTime -
        minutes * 60.0;

    wchar_t timeText[64];

    swprintf_s(
        timeText,
        L"%02d:%06.3f",
        minutes,
        seconds
    );

    RECT timeRc{
        0,
        125,
        VIDEO_WIDTH,
        180
    };

    DrawTextCentered(
        hdc,
        timeText,
        timeRc,
        30,
        RGB(
            180,
            180,
            190
        )
    );

    // --------------------------------------------------------
    // Keyboard
    // --------------------------------------------------------

    const int keyW = 82;
    const int keyH = 68;
    const int gap = 8;

    const int startX = 80;
    const int startY = 260;

    // Number row

    const wchar_t* numberKeys[] =
    {
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

    for (int i = 0; i < 10; ++i)
    {
        DrawKey(
            hdc,
            startX +
                i * (keyW + gap),
            startY,
            keyW,
            keyH,
            numberKeys[i],
            numberKeys[i]
        );
    }

    // QWERTY

    const wchar_t* row2[] =
    {
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

    for (int i = 0; i < 10; ++i)
    {
        DrawKey(
            hdc,
            startX +
                35 +
                i * (keyW + gap),
            startY +
                keyH +
                gap,
            keyW,
            keyH,
            row2[i],
            row2[i]
        );
    }

    // ASDF

    const wchar_t* row3[] =
    {
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

    for (int i = 0; i < 9; ++i)
    {
        DrawKey(
            hdc,
            startX +
                75 +
                i * (keyW + gap),
            startY +
                2 * (keyH + gap),
            keyW,
            keyH,
            row3[i],
            row3[i]
        );
    }

    // Shift + ZXCVBNM

    DrawKey(
        hdc,
        startX,
        startY +
            3 * (keyH + gap),
        150,
        keyH,
        L"Left Shift",
        L"SHIFT"
    );

    const wchar_t* row4[] =
    {
        L"Z",
        L"X",
        L"C",
        L"V",
        L"B",
        L"N",
        L"M"
    };

    for (int i = 0; i < 7; ++i)
    {
        DrawKey(
            hdc,
            startX +
                158 +
                i * (keyW + gap),
            startY +
                3 * (keyH + gap),
            keyW,
            keyH,
            row4[i],
            row4[i]
        );
    }

    // Space

    DrawKey(
        hdc,
        startX + 300,
        startY +
            4 * (keyH + gap),
        550,
        keyH,
        L"Space",
        L"SPACE"
    );

    // Enter

    DrawKey(
        hdc,
        startX +
            9 * (keyW + gap) -
            10,
        startY +
            2 * (keyH + gap),
        135,
        keyH,
        L"Enter",
        L"ENTER"
    );

    // Ctrl / Alt / Tab

    DrawKey(
        hdc,
        startX,
        startY +
            4 * (keyH + gap),
        125,
        keyH,
        L"Left Ctrl",
        L"CTRL"
    );

    DrawKey(
        hdc,
        startX + 135,
        startY +
            4 * (keyH + gap),
        130,
        keyH,
        L"Left Alt",
        L"ALT"
    );

    DrawKey(
        hdc,
        startX + 850,
        startY +
            4 * (keyH + gap),
        130,
        keyH,
        L"Tab",
        L"TAB"
    );

    // --------------------------------------------------------
    // Mouse
    // --------------------------------------------------------

    int mouseX = 1250;
    int mouseY = 320;

    FillRectColor(
        hdc,
        mouseX,
        mouseY,
        mouseX + 400,
        mouseY + 350,
        RGB(
            28,
            28,
            34
        )
    );

    RECT mouseTitle{
        mouseX,
        mouseY + 20,
        mouseX + 400,
        mouseY + 70
    };

    DrawTextCentered(
        hdc,
        L"MOUSE",
        mouseTitle,
        28,
        RGB(
            190,
            190,
            200
        )
    );

    DrawKey(
        hdc,
        mouseX + 40,
        mouseY + 105,
        145,
        100,
        L"LMB",
        L"LMB"
    );

    DrawKey(
        hdc,
        mouseX + 215,
        mouseY + 105,
        145,
        100,
        L"RMB",
        L"RMB"
    );

    // --------------------------------------------------------
    // Progress bar
    // --------------------------------------------------------

    const int barX = 80;
    const int barY = 990;
    const int barW = 1760;
    const int barH = 18;

    FillRectColor(
        hdc,
        barX,
        barY,
        barX + barW,
        barY + barH,
        RGB(
            50,
            50,
            58
        )
    );

    double total =
        duration +
        TAIL_TIME;

    double progress = 0.0;

    if (total > 0.0)
    {
        progress =
            currentTime /
            total;
    }

    if (progress < 0.0)
        progress = 0.0;

    if (progress > 1.0)
        progress = 1.0;

    FillRectColor(
        hdc,
        barX,
        barY,
        barX +
            static_cast<int>(
                barW * progress
            ),
        barY + barH,
        RGB(
            230,
            145,
            40
        )
    );
}


// ============================================================
// CREATE DIB
// ============================================================

struct FrameBuffer
{
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    HDC dc = nullptr;
};


bool CreateFrameBuffer(
    FrameBuffer& frame
)
{
    HDC screen =
        GetDC(nullptr);

    frame.dc =
        CreateCompatibleDC(
            screen
        );

    BITMAPINFO bmi{};

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

    frame.bitmap =
        CreateDIBSection(
            screen,
            &bmi,
            DIB_RGB_COLORS,
            &frame.bits,
            nullptr,
            0
        );

    ReleaseDC(
        nullptr,
        screen
    );

    if (
        !frame.dc ||
        !frame.bitmap ||
        !frame.bits
    )
    {
        return false;
    }

    SelectObject(
        frame.dc,
        frame.bitmap
    );

    return true;
}


void DestroyFrameBuffer(
    FrameBuffer& frame
)
{
    if (frame.bitmap)
    {
        DeleteObject(
            frame.bitmap
        );

        frame.bitmap =
            nullptr;
    }

    if (frame.dc)
    {
        DeleteDC(
            frame.dc
        );

        frame.dc =
            nullptr;
    }

    frame.bits =
        nullptr;
}


// ============================================================
// MEDIA FOUNDATION EXPORT
// ============================================================

bool ExportMP4()
{
    if (events.empty())
        return false;

    HRESULT hr =
        MFStartup(
            MF_VERSION
        );

    if (FAILED(hr))
        return false;

    IMFAttributes* attributes =
        nullptr;

    IMFMediaType* outputType =
        nullptr;

    IMFMediaType* inputType =
        nullptr;

    IMFSourceReader* reader =
        nullptr;

    IMFSinkWriter* writer =
        nullptr;

    DWORD streamIndex = 0;

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

        // ----------------------------------------------------
        // H264 OUTPUT
        // ----------------------------------------------------

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

        hr =
            writer->AddStream(
                outputType,
                &streamIndex
            );

        if (FAILED(hr))
            break;

        // ----------------------------------------------------
        // RGB INPUT
        // ----------------------------------------------------

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
            break;

        hr =
            writer->BeginWriting();

        if (FAILED(hr))
            break;

        // ----------------------------------------------------
        // FRAME BUFFER
        // ----------------------------------------------------

        FrameBuffer frame;

        if (
            !CreateFrameBuffer(
                frame
            )
        )
        {
            break;
        }

        double total =
            duration +
            TAIL_TIME;

        LONGLONG frameDuration =
            10000000LL /
            VIDEO_FPS;

        LONGLONG frameCount =
            static_cast<LONGLONG>(
                std::ceil(
                    total *
                    VIDEO_FPS
                )
            );

        bool frameSuccess = true;

        for (
            LONGLONG i = 0;
            i < frameCount;
            ++i
        )
        {
            currentTime =
                static_cast<double>(
                    i
                ) /
                VIDEO_FPS;

            // Reset and replay to this frame.
            buttonStates.clear();
            actionTimes.clear();
            nextEvent = 0;

            ProcessEventsTo(
                currentTime
            );

            RenderScene(
                frame.dc
            );

            IMFSample* sample =
                nullptr;

            IMFMediaBuffer* buffer =
                nullptr;

            hr =
                MFCreateMemoryBuffer(
                    VIDEO_WIDTH *
                    VIDEO_HEIGHT *
                    4,
                    &buffer
                );

            if (FAILED(hr))
            {
                frameSuccess = false;
                break;
            }

            BYTE* destination =
                nullptr;

            DWORD maxLength = 0;
            DWORD currentLength = 0;

            hr =
                buffer->Lock(
                    &destination,
                    &maxLength,
                    &currentLength
                );

            if (SUCCEEDED(hr))
            {
                memcpy(
                    destination,
                    frame.bits,
                    VIDEO_WIDTH *
                    VIDEO_HEIGHT *
                    4
                );

                buffer->Unlock();

                buffer->SetCurrentLength(
                    VIDEO_WIDTH *
                    VIDEO_HEIGHT *
                    4
                );

                hr =
                    MFCreateSample(
                        &sample
                    );

                if (SUCCEEDED(hr))
                {
                    sample->AddBuffer(
                        buffer
                    );

                    sample->SetSampleTime(
                        i *
                        frameDuration
                    );

                    sample->SetSampleDuration(
                        frameDuration
                    );

                    hr =
                        writer->WriteSample(
                            streamIndex,
                            sample
                        );
                }
            }

            if (sample)
                sample->Release();

            if (buffer)
                buffer->Release();

            if (FAILED(hr))
            {
                frameSuccess = false;
                break;
            }
        }

        DestroyFrameBuffer(
            frame
        );

        if (!frameSuccess)
            break;

        hr =
            writer->Finalize();

        if (FAILED(hr))
            break;

        success = true;

    } while (false);

    if (writer)
        writer->Release();

    if (reader)
        reader->Release();

    if (inputType)
        inputType->Release();

    if (outputType)
        outputType->Release();

    if (attributes)
        attributes->Release();

    MFShutdown();

    return success;
}


// ============================================================
// EXPORT THREAD
// ============================================================

DWORD WINAPI ExportThreadProc(
    LPVOID
)
{
    bool success =
        ExportMP4();

    PostMessageW(
        hwnd,
        WM_EXPORT_DONE,
        success
            ? 1
            : 0,
        0
    );

    return 0;
}


void StartExport()
{
    if (exporting)
        return;

    if (events.empty())
    {
        SetWindowTextW(
            statusText,
            L"Load a timeline first."
        );

        return;
    }

    wchar_t exePath[MAX_PATH];

    DWORD length =
        GetModuleFileNameW(
            nullptr,
            exePath,
            MAX_PATH
        );

    std::wstring directory(
        exePath,
        length
    );

    size_t slash =
        directory.find_last_of(
            L"\\/"
        );

    if (
        slash !=
        std::wstring::npos
    )
    {
        directory =
            directory.substr(
                0,
                slash
            );
    }

    outputPath =
        directory +
        L"\\APM_Replay.mp4";

    exporting = true;

    SetWindowTextW(
        statusText,
        L"Exporting MP4..."
    );

    EnableWindow(
        timelineEdit,
        FALSE
    );

    EnableWindow(
        GetDlgItem(
            hwnd,
            ID_LOAD
        ),
        FALSE
    );

    EnableWindow(
        playButton,
        FALSE
    );

    EnableWindow(
        GetDlgItem(
            hwnd,
            ID_RESET
        ),
        FALSE
    );

    EnableWindow(
        GetDlgItem(
            hwnd,
            ID_EXPORT
        ),
        FALSE
    );

    exportThread =
        CreateThread(
            nullptr,
            0,
            ExportThreadProc,
            nullptr,
            0,
            nullptr
        );

    if (!exportThread)
    {
        exporting = false;

        EnableWindow(
            timelineEdit,
            TRUE
        );

        EnableWindow(
            GetDlgItem(
                hwnd,
                ID_LOAD
            ),
            TRUE
        );

        EnableWindow(
            playButton,
            TRUE
        );

        EnableWindow(
            GetDlgItem(
                hwnd,
                ID_RESET
            ),
            TRUE
        );

        EnableWindow(
            GetDlgItem(
                hwnd,
                ID_EXPORT
            ),
            TRUE
        );

        SetWindowTextW(
            statusText,
            L"Could not start export."
        );
    }
}


// ============================================================
// PREVIEW
// ============================================================

void DrawPreview(
    HDC hdc,
    RECT client
)
{
    int width =
        client.right -
        client.left;

    int height =
        client.bottom -
        client.top;

    FillRectColor(
        hdc,
        0,
        0,
        width,
        height,
        RGB(
            12,
            12,
            15
        )
    );

    if (width <= 20 ||
        height <= 20)
    {
        return;
    }

    double videoAspect =
        static_cast<double>(
            VIDEO_WIDTH
        ) /
        VIDEO_HEIGHT;

    double clientAspect =
        static_cast<double>(
            width
        ) /
        height;

    int drawW;
    int drawH;

    if (
        clientAspect >
        videoAspect
    )
    {
        drawH =
            height;

        drawW =
            static_cast<int>(
                drawH *
                videoAspect
            );
    }
    else
    {
        drawW =
            width;

        drawH =
            static_cast<int>(
                drawW /
                videoAspect
            );
    }

    int x =
        (width - drawW) /
        2;

    int y =
        (height - drawH) /
        2;

    HDC memoryDC =
        CreateCompatibleDC(
            hdc
        );

    BITMAPINFO bmi{};

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
            hdc,
            &bmi,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0
        );

    if (!bitmap)
    {
        DeleteDC(
            memoryDC
        );

        return;
    }

    HBITMAP old =
        (HBITMAP)SelectObject(
            memoryDC,
            bitmap
        );

    RenderScene(
        memoryDC
    );

    SetStretchBltMode(
        hdc,
        HALFTONE
    );

    StretchBlt(
        hdc,
        x,
        y,
        drawW,
        drawH,
        memoryDC,
        0,
        0,
        VIDEO_WIDTH,
        VIDEO_HEIGHT,
        SRCCOPY
    );

    SelectObject(
        memoryDC,
        old
    );

    DeleteObject(
        bitmap
    );

    DeleteDC(
        memoryDC
    );
}


// ============================================================
// WINDOW PROCEDURE
// ============================================================

LRESULT CALLBACK WndProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            timelineEdit =
                CreateWindowExW(
                    WS_EX_CLIENTEDGE,
                    L"EDIT",
                    L"",
                    WS_CHILD |
                    WS_VISIBLE |
                    ES_MULTILINE |
                    ES_AUTOVSCROLL |
                    ES_AUTOHSCROLL |
                    WS_VSCROLL |
                    WS_HSCROLL,
                    20,
                    20,
                    1360,
                    260,
                    hWnd,
                    (HMENU)ID_EDIT,
                    GetModuleHandleW(
                        nullptr
                    ),
                    nullptr
                );

            SendMessageW(
                timelineEdit,
                EM_SETLIMITTEXT,
                2 * 1024 * 1024,
                0
            );

            // Buttons

            CreateWindowW(
                L"BUTTON",
                L"Load Timeline",
                WS_CHILD |
                WS_VISIBLE |
                BS_PUSHBUTTON,
                20,
                295,
                150,
                38,
                hWnd,
                (HMENU)ID_LOAD,
                GetModuleHandleW(
                    nullptr
                ),
                nullptr
            );

            playButton =
                CreateWindowW(
                    L"BUTTON",
                    L"Play",
                    WS_CHILD |
                    WS_VISIBLE |
                    BS_PUSHBUTTON,
                    180,
                    295,
                    100,
                    38,
                    hWnd,
                    (HMENU)ID_PLAY,
                    GetModuleHandleW(
                        nullptr
                    ),
                    nullptr
                );

            CreateWindowW(
                L"BUTTON",
                L"Reset",
                WS_CHILD |
                WS_VISIBLE |
                BS_PUSHBUTTON,
                290,
                295,
                100,
                38,
                hWnd,
                (HMENU)ID_RESET,
                GetModuleHandleW(
                    nullptr
                ),
                nullptr
            );

            CreateWindowW(
                L"BUTTON",
                L"Export MP4",
                WS_CHILD |
                WS_VISIBLE |
                BS_PUSHBUTTON,
                400,
                295,
                120,
                38,
                hWnd,
                (HMENU)ID_EXPORT,
                GetModuleHandleW(
                    nullptr
                ),
                nullptr
            );

            statusText =
                CreateWindowW(
                    L"STATIC",
                    L"Paste your timeline above, then click Load Timeline.",
                    WS_CHILD |
                    WS_VISIBLE,
                    540,
                    300,
                    800,
                    30,
                    hWnd,
                    nullptr,
                    GetModuleHandleW(
                        nullptr
                    ),
                    nullptr
                );

            SetTimer(
                hWnd,
                1,
                16,
                nullptr
            );

            return 0;
        }

        case WM_COMMAND:
        {
            switch (
                LOWORD(wParam)
            )
            {
                case ID_LOAD:
                    LoadTimeline();
                    break;

                case ID_PLAY:
                {
                    if (events.empty())
                    {
                        SetWindowTextW(
                            statusText,
                            L"Load a timeline first."
                        );

                        break;
                    }

                    if (
                        currentTime >=
                        duration +
                        TAIL_TIME
                    )
                    {
                        ResetSimulation();
                    }

                    playing =
                        !playing;

                    SetWindowTextW(
                        playButton,
                        playing
                            ? L"Pause"
                            : L"Play"
                    );

                    break;
                }

                case ID_RESET:
                    ResetSimulation();
                    break;

                case ID_EXPORT:
                    StartExport();
                    break;
            }

            return 0;
        }

        case WM_TIMER:
        {
            if (
                wParam == 1 &&
                playing &&
                !exporting
            )
            {
                currentTime +=
                    1.0 /
                    VIDEO_FPS;

                ProcessEventsTo(
                    currentTime
                );

                if (
                    currentTime >=
                    duration +
                    TAIL_TIME
                )
                {
                    currentTime =
                        duration +
                        TAIL_TIME;

                    playing =
                        false;

                    SetWindowTextW(
                        playButton,
                        L"Play"
                    );
                }

                InvalidateRect(
                    hWnd,
                    nullptr,
                    FALSE
                );
            }

            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;

            HDC hdc =
                BeginPaint(
                    hWnd,
                    &ps
                );

            RECT rc;

            GetClientRect(
                hWnd,
                &rc
            );

            RECT preview =
                rc;

            preview.top =
                345;

            DrawPreview(
                hdc,
                preview
            );

            EndPaint(
                hWnd,
                &ps
            );

            return 0;
        }

        case WM_SIZE:
        {
            if (timelineEdit)
            {
                int width =
                    LOWORD(lParam);

                int height =
                    HIWORD(lParam);

                int editWidth =
                    max(
                        300,
                        width - 40
                    );

                MoveWindow(
                    timelineEdit,
                    20,
                    20,
                    editWidth,
                    260,
                    TRUE
                );

                HWND load =
                    GetDlgItem(
                        hWnd,
                        ID_LOAD
                    );

                HWND play =
                    GetDlgItem(
                        hWnd,
                        ID_PLAY
                    );

                HWND reset =
                    GetDlgItem(
                        hWnd,
                        ID_RESET
                    );

                HWND exportButton =
                    GetDlgItem(
                        hWnd,
                        ID_EXPORT
                    );

                if (load)
                    MoveWindow(
                        load,
                        20,
                        295,
                        150,
                        38,
                        TRUE
                    );

                if (play)
                    MoveWindow(
                        play,
                        180,
                        295,
                        100,
                        38,
                        TRUE
                    );

                if (reset)
                    MoveWindow(
                        reset,
                        290,
                        295,
                        100,
                        38,
                        TRUE
                    );

                if (exportButton)
                    MoveWindow(
                        exportButton,
                        400,
                        295,
                        120,
                        38,
                        TRUE
                    );

                if (statusText)
                {
                    MoveWindow(
                        statusText,
                        540,
                        300,
                        max(
                            300,
                            width - 560
                        ),
                        30,
                        TRUE
                    );
                }
            }

            return 0;
        }

        case WM_EXPORT_DONE:
        {
            exporting = false;

            if (exportThread)
            {
                CloseHandle(
                    exportThread
                );

                exportThread =
                    nullptr;
            }

            EnableWindow(
                timelineEdit,
                TRUE
            );

            EnableWindow(
                GetDlgItem(
                    hWnd,
                    ID_LOAD
                ),
                TRUE
            );

            EnableWindow(
                playButton,
                TRUE
            );

            EnableWindow(
                GetDlgItem(
                    hWnd,
                    ID_RESET
                ),
                TRUE
            );

            EnableWindow(
                GetDlgItem(
                    hWnd,
                    ID_EXPORT
                ),
                TRUE
            );

            if (wParam)
            {
                SetWindowTextW(
                    statusText,
                    L"MP4 exported: APM_Replay.mp4"
                );
            }
            else
            {
                SetWindowTextW(
                    statusText,
                    L"MP4 export failed."
                );
            }

            return 0;
        }

        case WM_DESTROY:
        {
            KillTimer(
                hWnd,
                1
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
    }

    return DefWindowProcW(
        hWnd,
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
    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.hInstance =
        hInstance;

    wc.lpfnWndProc =
        WndProc;

    wc.lpszClassName =
        L"APMTimelineVisualizer";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        (HBRUSH)(
            COLOR_WINDOW + 1
        );

    RegisterClassExW(
        &wc
    );

    hwnd =
        CreateWindowExW(
            0,
            wc.lpszClassName,
            L"APM Timeline Visualizer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1420,
            900,
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
