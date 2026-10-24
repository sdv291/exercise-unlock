// ExerciseUnlockCalibrate — measures the child's knee / elbow angles from
// four photos (standing, squat, push-up up, push-up down) and writes the
// rep-detection thresholds into config.ini.
//
//   ExerciseUnlockCalibrate.exe [/lang en|uk] [/setup]
//
//   /setup  launched by the installer before the service starts: after
//           saving, don't restart the service (setup starts it).
//
// Angles are measured exactly the way SquatDetector / PushUpDetector see
// them: same MoveNet model (PoseEstimator.cpp), same camera format (RGB32
// at the camera's default size — angles are taken in normalized frame
// coordinates, so the aspect ratio must match the service's), same joints,
// same confidence gates, same "is this a push-up at all" checks.

#define NOMINMAX
#include "PoseEstimator.h"
#include "Log.h"
#include "Paths.h"

#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <objidl.h>
// gdiplus.h calls unqualified min / max, which NOMINMAX hides.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using ExerciseUnlock::Svc::BodyPose;
using ExerciseUnlock::Svc::Joint;
using ExerciseUnlock::Svc::LogF;
using ExerciseUnlock::Svc::PoseEstimator;

namespace
{
    // ------------------------------------------------------------ text
    enum Txt
    {
        tWindow, tCamera, tBack, tNext, tTake, tRetake, tSkip, tSave, tClose, tStepOf,
        tTitle0, tText0, tTitle1, tText1, tTitle2, tText2, tTitle3, tText3, tTitle4, tText4,
        tTitle5, tText5,
        tRow1, tRow2, tRow3, tRow4, tKnee, tElbow, tSkipped, tNotYet,
        tLegsNotSeen, tPushNotSeen, tSmallSquat, tSmallPush, tNoCamera, tCamBusy, tNoFrames, tNoModel,
        tSaved, tRestarted, tRestartFailed, tSaveFailed, tNothingToSave,
        tSumSquat, tSumPush, tSumHip, tSumKept, tSumCamera,
        tExampleSetup, tExampleView, tDistance,
        tCount
    };

    const wchar_t* const kEn[] = {
        L"Exercise Unlock - calibration", L"Camera:", L"Back", L"Next", L"Take photo", L"Retake", L"Skip", L"Save", L"Close",
        L"Step %d of %d",
        L"Camera",
        L"Put the camera where the child will exercise and pick it in the list above.\n\n"
        L"The child must be fully visible from the side - from head to feet. For push-ups the camera has to stand on the floor.\n\n"
        L"Next you will take 4 photos: standing, squatting, push-up with straight arms and push-up at the bottom. "
        L"Exercise Unlock measures the child's angles and tunes the rep counting to them.",
        L"1. Standing, side view",
        L"The child stands sideways to the camera at full height: legs straight, arms down.\n\n"
        L"Press \"Take photo\" - the photo is taken right away.",
        L"2. Squat, side view",
        L"The child squats as deep as in a normal rep and holds still, still sideways to the camera.\n\n"
        L"Press \"Take photo\" and keep the squat until the photo appears.",
        L"3. Push-up, arms straight",
        L"Put the camera on the floor, 2-3 m away. The child takes the plank position sideways to the camera, arms straight.\n\n"
        L"Press \"Take photo\".",
        L"4. Push-up, bottom",
        L"The child lowers down as in a normal push-up - elbows bent - and holds still.\n\n"
        L"Press \"Take photo\".",
        L"Result",
        L"These thresholds will be written to config.ini. Exercises whose photos were skipped keep their current settings.",
        L"Standing", L"Squat", L"Push-up up", L"Push-up down", L"knee %d\x00B0", L"elbow %d\x00B0", L"skipped", L"-",
        L"The child is not fully visible: hips, knees and ankles must be in the frame. Check the camera and take the photo again.",
        L"No push-up position recognized: shoulders, elbows, wrists, hips and ankles must be visible and the hands on the floor. Put the camera on the floor and try again.",
        L"Standing and squat differ by only %d\x00B0 - squat deeper or check the camera angle. Squat settings were not changed.",
        L"Straight and bent arms differ by only %d\x00B0 - lower deeper or check the camera angle. Push-up settings were not changed.",
        L"No camera found. Connect a camera and start the calibration again.",
        L"The camera is busy or unavailable. Close other apps that use it and pick the camera again.",
        L"The camera is not sending a picture. Pick another camera in the list above, or close other apps that use it.",
        L"The pose model could not be loaded - is the installation complete?",
        L"Saved to config.ini.", L" The service was restarted.", L" Restart the service (or the computer) to apply.",
        L"Could not write config.ini (error %lu). Run the calibration as administrator.",
        L"Nothing to save - all photos were skipped.",
        L"Squats (knee angle)\n  standing %s \x2192 %d, going down %s \x2192 %d\n  going up %s \x2192 %d, down %s \x2192 %d",
        L"Push-ups (elbow angle)\n  up %s \x2192 %d, going down %s \x2192 %d\n  going up %s \x2192 %d, down %s \x2192 %d",
        L"Minimum hip drop %s \x2192 %.2f",
        L"%s: kept as is",
        L"Camera: %s",
        L"Example - where to put the camera:",
        L"Example - what the camera should see:",
        L"2-3 m",
    };

    const wchar_t* const kUk[] = {
        L"Exercise Unlock - калібрування", L"Камера:", L"Назад", L"Далі", L"Зробити фото", L"Перезняти", L"Пропустити", L"Зберегти", L"Закрити",
        L"Крок %d з %d",
        L"Камера",
        L"Поставте камеру там, де дитина виконуватиме вправи, і виберіть її у списку вище.\n\n"
        L"Дитину має бути видно збоку повністю - від голови до ніг. Для віджимань камера має стояти на підлозі.\n\n"
        L"Далі ви зробите 4 фото: стоячи, у присіді, віджимання з прямими руками та віджимання внизу. "
        L"Exercise Unlock виміряє кути дитини й налаштує під них підрахунок повторів.",
        L"1. Стоячи, у профіль",
        L"Дитина стоїть боком до камери на повний зріст: ноги прямі, руки опущені.\n\n"
        L"Натисніть «Зробити фото» - знімок буде зроблено одразу.",
        L"2. Присід, у профіль",
        L"Дитина присідає так глибоко, як у звичайному повторенні, і завмирає - так само боком до камери.\n\n"
        L"Натисніть «Зробити фото» й тримайте присід, доки не з'явиться знімок.",
        L"3. Віджимання, руки прямі",
        L"Поставте камеру на підлогу за 2-3 м. Дитина стає в планку боком до камери, руки прямі.\n\n"
        L"Натисніть «Зробити фото».",
        L"4. Віджимання, внизу",
        L"Дитина опускається, як у звичайному віджиманні, - лікті зігнуті - і завмирає.\n\n"
        L"Натисніть «Зробити фото».",
        L"Результат",
        L"Ці пороги буде записано в config.ini. Вправи, фото яких пропущено, зберігають поточні налаштування.",
        L"Стоячи", L"Присід", L"Віджимання вгорі", L"Віджимання внизу", L"коліно %d\x00B0", L"лікоть %d\x00B0", L"пропущено", L"-",
        L"Дитину видно не повністю: стегна, коліна й щиколотки мають бути в кадрі. Перевірте камеру й зробіть фото ще раз.",
        L"Положення віджимання не розпізнано: мають бути видні плечі, лікті, кисті, стегна й щиколотки, а руки - на підлозі. Поставте камеру на підлогу й спробуйте ще раз.",
        L"Стійка й присід відрізняються лише на %d\x00B0 - присядьте глибше або перевірте ракурс камери. Налаштування присідань не змінено.",
        L"Прямі й зігнуті руки відрізняються лише на %d\x00B0 - опустіться нижче або перевірте ракурс камери. Налаштування віджимань не змінено.",
        L"Камеру не знайдено. Підключіть камеру й запустіть калібрування знову.",
        L"Камера зайнята або недоступна. Закрийте інші програми, що її використовують, і виберіть камеру ще раз.",
        L"Камера не передає зображення. Виберіть іншу камеру у списку вище або закрийте інші програми, що її використовують.",
        L"Не вдалося завантажити модель пози - чи завершено встановлення?",
        L"Збережено в config.ini.", L" Службу перезапущено.", L" Перезапустіть службу (або комп'ютер), щоб застосувати.",
        L"Не вдалося записати config.ini (помилка %lu). Запустіть калібрування від імені адміністратора.",
        L"Нічого зберігати - усі фото пропущено.",
        L"Присідання (кут коліна)\n  стійка %s \x2192 %d, спуск %s \x2192 %d\n  підйом %s \x2192 %d, низ %s \x2192 %d",
        L"Віджимання (кут ліктя)\n  угорі %s \x2192 %d, спуск %s \x2192 %d\n  підйом %s \x2192 %d, низ %s \x2192 %d",
        L"Мінімальне опускання стегон %s \x2192 %.2f",
        L"%s: без змін",
        L"Камера: %s",
        L"Приклад - куди поставити камеру:",
        L"Приклад - так має бачити камера:",
        L"2-3 м",
    };

    static_assert(std::size(kEn) == tCount && std::size(kUk) == tCount, "string tables out of sync");

    bool g_uk = false;
    const wchar_t* T(Txt id) { return (g_uk ? kUk : kEn)[id]; }

    std::wstring Fmt(const wchar_t* fmt, ...)
    {
        wchar_t buf[1024]{};
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
        va_end(args);
        return buf;
    }

    // ------------------------------------------------------------ measurements
    // Joint indices + confidence gates copied from the detectors.
    constexpr int kLSh = 5, kRSh = 6, kLEl = 7, kREl = 8, kLWr = 9, kRWr = 10;
    constexpr int kLHip = 11, kRHip = 12, kLKn = 13, kRKn = 14, kLAn = 15, kRAn = 16;
    constexpr float kSquatConf = 0.22f;   // SquatDetector::kJointConfMin
    constexpr float kPushConf  = 0.18f;   // PushUpDetector::kJointConfMin

    float AngleAt(const Joint& a, const Joint& b, const Joint& c)
    {
        const float ax = a.x - b.x, ay = a.y - b.y;
        const float cx = c.x - b.x, cy = c.y - b.y;
        const float mag = std::sqrt(ax * ax + ay * ay) * std::sqrt(cx * cx + cy * cy);
        if (mag < 1e-6f) return 0.0f;
        const float cosine = std::clamp((ax * cx + ay * cy) / mag, -1.0f, 1.0f);
        return std::acos(cosine) * (180.0f / 3.14159265358979323846f);
    }

    bool Conf3(const Joint& a, const Joint& b, const Joint& c, float minConf)
    {
        return a.confidence >= minConf && b.confidence >= minConf && c.confidence >= minConf;
    }

    // Knee angle the way SquatDetector sees it: max over confident legs.
    float KneeAngle(const BodyPose& p)
    {
        float best = -1.0f;
        if (Conf3(p.joints[kLHip], p.joints[kLKn], p.joints[kLAn], kSquatConf))
            best = std::max(best, AngleAt(p.joints[kLHip], p.joints[kLKn], p.joints[kLAn]));
        if (Conf3(p.joints[kRHip], p.joints[kRKn], p.joints[kRAn], kSquatConf))
            best = std::max(best, AngleAt(p.joints[kRHip], p.joints[kRKn], p.joints[kRAn]));
        return best;
    }

    float HipY(const BodyPose& p)
    {
        float sum = 0.0f;
        int n = 0;
        for (int i : { kLHip, kRHip })
            if (p.joints[i].confidence >= kSquatConf) { sum += p.joints[i].y; ++n; }
        return n ? sum / static_cast<float>(n) : -1.0f;
    }

    // Elbow angle the way PushUpDetector sees it (average of confident
    // arms), or -1 when its hard gate (elbow + plank triplet + hands near
    // ankle height) would skip this frame.
    float PushUpElbow(const BodyPose& p)
    {
        float sum = 0.0f;
        int n = 0;
        if (Conf3(p.joints[kLSh], p.joints[kLEl], p.joints[kLWr], kPushConf))
        { sum += AngleAt(p.joints[kLSh], p.joints[kLEl], p.joints[kLWr]); ++n; }
        if (Conf3(p.joints[kRSh], p.joints[kREl], p.joints[kRWr], kPushConf))
        { sum += AngleAt(p.joints[kRSh], p.joints[kREl], p.joints[kRWr]); ++n; }
        if (n == 0) return -1.0f;

        const bool plank = Conf3(p.joints[kLSh], p.joints[kLHip], p.joints[kLAn], kPushConf) ||
                           Conf3(p.joints[kRSh], p.joints[kRHip], p.joints[kRAn], kPushConf);
        if (!plank) return -1.0f;

        auto wristY = [&](int sh, int wr) -> float {
            if (p.joints[wr].confidence >= kPushConf) return p.joints[wr].y;
            if (p.joints[sh].confidence >= kPushConf) return p.joints[sh].y;
            return -1.0f;
        };
        auto pick = [](float a, float b) { return (a >= 0 && b >= 0) ? (a + b) * 0.5f : (a >= 0 ? a : b); };
        const float wrist = pick(wristY(kLSh, kLWr), wristY(kRSh, kRWr));
        const float ankle = pick(p.joints[kLAn].confidence >= kPushConf ? p.joints[kLAn].y : -1.0f,
                                 p.joints[kRAn].confidence >= kPushConf ? p.joints[kRAn].y : -1.0f);
        if (wrist < 0 || ankle < 0 || std::fabs(wrist - ankle) >= 0.25f) return -1.0f;
        return sum / static_cast<float>(n);
    }

    float Median(std::vector<float> v)
    {
        if (v.empty()) return -1.0f;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    // ------------------------------------------------------------ state
    struct Frame
    {
        std::vector<uint8_t> bgra;   // top-down BGRA
        uint32_t w = 0, h = 0;
        BodyPose pose{};
    };

    struct Shot
    {
        bool done = false, skipped = false;
        float angle = -1.0f;   // knee (steps 1-2) or elbow (3-4)
        float hipY  = -1.0f;   // steps 1-2
        Frame photo;
    };

    struct CameraInfo { std::wstring name, link; };

    // "Take photo" measures straight away: the parent presses it while the
    // child holds the pose, so there is no countdown.
    enum class Phase { Live, Collecting };

    constexpr int kSteps = 6;          // 0 camera, 1..4 photos, 5 summary
    constexpr UINT WM_APP_FRAME  = WM_APP + 1;
    constexpr UINT WM_APP_CAMERR = WM_APP + 2;
    constexpr UINT_PTR kTimerId  = 1;
    constexpr DWORD kCollectMs   = 5000;
    constexpr size_t kSamples    = 12;

    enum CtlId { idCamera = 100, idBack, idTake, idSkip, idNext };

    HINSTANCE g_inst = nullptr;
    HWND g_wnd = nullptr, g_cam = nullptr, g_back = nullptr, g_take = nullptr, g_skip = nullptr, g_next = nullptr;
    HFONT g_fontUi = nullptr, g_fontTitle = nullptr;
    UINT g_dpi = 96;
    bool g_setupMode = false;
    std::wstring g_snapshotPath;      // developer option: render one step to a .bmp and exit
    int g_snapshotStep = 0;
    constexpr UINT WM_APP_SNAPSHOT = WM_APP + 3;
    constexpr UINT_PTR kSnapshotTimerId = 2;

    std::wstring g_exeDir, g_configPath;
    PoseEstimator g_pose;
    bool g_poseOk = false;
    std::vector<CameraInfo> g_cameras;

    std::mutex g_frameMx;
    Frame g_latest;
    std::atomic<bool> g_framePending{ false };

    // One per opened camera. The UI thread never waits on a camera call:
    // a synchronous ReadSample blocks for good when the device sends no
    // frames, so StopCamera shuts the source down (ReadSample then
    // returns) and leaves a thread that still doesn't finish behind.
    struct CamSession
    {
        std::atomic<bool> stop{ false };
        std::mutex mx;
        IMFMediaSource* source = nullptr;   // AddRef'd while open, under mx
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~CamSession() { if (done) CloseHandle(done); }
    };
    std::shared_ptr<CamSession> g_camSession;
    std::thread g_camThread;
    bool g_camStuck = false;                  // a camera thread was left behind
    std::atomic<ULONGLONG> g_lastFrameMs{ 0 };
    constexpr ULONGLONG kNoFramesMs = 10000;

    int g_step = 0;
    Phase g_phase = Phase::Live;
    ULONGLONG g_phaseStart = 0;
    std::vector<float> g_samples, g_hipSamples;
    Shot g_shots[5];
    std::wstring g_status;            // error / info line under the instructions
    bool g_statusIsError = false;
    bool g_saved = false;

    int Px(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }

    // ------------------------------------------------------------ config
    std::wstring IniGet(const wchar_t* sec, const wchar_t* key)
    {
        wchar_t buf[256]{};
        GetPrivateProfileStringW(sec, key, L"", buf, static_cast<DWORD>(std::size(buf)), g_configPath.c_str());
        return buf;
    }

    // ------------------------------------------------------------ camera
    std::vector<CameraInfo> EnumerateCameras()
    {
        std::vector<CameraInfo> out;
        IMFAttributes* attrs = nullptr;
        IMFActivate** devs = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) &&
            SUCCEEDED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) &&
            SUCCEEDED(MFEnumDeviceSources(attrs, &devs, &count)))
        {
            for (UINT32 i = 0; i < count; ++i)
            {
                wchar_t* name = nullptr;
                wchar_t* link = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)) &&
                    SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len)))
                {
                    out.push_back({ name, link });
                }
                if (name) CoTaskMemFree(name);
                if (link) CoTaskMemFree(link);
                devs[i]->Release();
            }
            CoTaskMemFree(devs);
        }
        if (attrs) attrs->Release();
        return out;
    }

    // Capture + pose loop. Same reader setup as the service's WebcamProbe
    // (RGB32 at the default frame size) so the measured angles match.
    void CameraThread(std::shared_ptr<CamSession> s, std::wstring link)
    {
        const ULONGLONG openedMs = GetTickCount64();
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMFAttributes* attrs = nullptr;
        IMFMediaSource* source = nullptr;
        IMFSourceReader* reader = nullptr;
        IMFAttributes* readerAttrs = nullptr;
        uint32_t w = 0, h = 0;

        HRESULT hr = MFCreateAttributes(&attrs, 2);
        if (SUCCEEDED(hr)) hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(hr)) hr = attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
        if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attrs, &source);
        if (SUCCEEDED(hr))
        {
            std::lock_guard<std::mutex> lk(s->mx);
            s->source = source;
            source->AddRef();
            if (s->stop.load()) source->Shutdown();   // stopped while opening
        }
        if (SUCCEEDED(hr)) hr = MFCreateAttributes(&readerAttrs, 1);
        if (SUCCEEDED(hr)) readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source, readerAttrs, &reader);
        if (SUCCEEDED(hr))
        {
            IMFMediaType* type = nullptr;
            hr = MFCreateMediaType(&type);
            if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type);
            if (type) type->Release();
        }
        if (SUCCEEDED(hr))
        {
            IMFMediaType* cur = nullptr;
            hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur);
            if (SUCCEEDED(hr)) { MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h); cur->Release(); }
        }
        LogF(L"calibrate: camera open hr=0x%08X size=%ux%u after %llu ms", static_cast<unsigned>(hr), w, h,
             GetTickCount64() - openedMs);
        if ((FAILED(hr) || w == 0 || h == 0) && !s->stop.load())
            PostMessageW(g_wnd, WM_APP_CAMERR, 0, 0);

        std::vector<uint8_t> buf;
        unsigned frames = 0, empty = 0, shown = 0, wrongSize = 0;
        ULONGLONG lastStatMs = GetTickCount64();
        while (SUCCEEDED(hr) && w && h && !s->stop.load())
        {
            IMFSample* sample = nullptr;
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &sample);
            if (s->stop.load())
            {
                if (sample) sample->Release();
                break;
            }
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            {
                LogF(L"calibrate: ReadSample failed hr=0x%08X flags=0x%X after %u frames",
                     static_cast<unsigned>(hr), flags, frames);
                if (sample) sample->Release();
                PostMessageW(g_wnd, WM_APP_CAMERR, 0, 0);
                break;
            }
            // The frame size can change once the stream runs (the camera
            // settles on another format). Keeping the size from open time
            // made every frame fail the size check below: no picture at all.
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            {
                IMFMediaType* cur = nullptr;
                if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur)))
                {
                    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
                    cur->Release();
                }
                LogF(L"calibrate: media type changed -> %ux%u", w, h);
            }
            // Until the first picture is on screen, say every 5 s where frames go.
            if (shown == 0 && GetTickCount64() - lastStatMs >= 5000)
            {
                lastStatMs = GetTickCount64();
                LogF(L"calibrate: nothing shown yet - %u frames read, %u wrong size, %u empty reads",
                     frames, wrongSize, empty);
            }
            if (!sample)
            {
                if (++empty <= 10) LogF(L"calibrate: ReadSample returned no sample (flags=0x%X)", flags);
                continue;
            }
            if (++frames == 1)
                LogF(L"calibrate: first frame after %llu ms", GetTickCount64() - openedMs);
            IMFMediaBuffer* mb = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&mb)) && mb)
            {
                BYTE* data = nullptr;
                DWORD maxLen = 0, curLen = 0;
                if (SUCCEEDED(mb->Lock(&data, &maxLen, &curLen)))
                {
                    const size_t need = static_cast<size_t>(w) * h * 4;
                    if (curLen < need)
                    {
                        if (++wrongSize <= 5)
                            LogF(L"calibrate: frame %u has %lu bytes, %ux%u needs %zu - dropped", frames, curLen, w, h, need);
                    }
                    else
                    {
                        buf.assign(data, data + need);
                        BodyPose pose{};
                        const ULONGLONG poseStart = GetTickCount64();
                        if (!g_poseOk || FAILED(g_pose.Predict(buf.data(), w, h, pose)))
                            pose = BodyPose{};
                        const ULONGLONG poseMs = GetTickCount64() - poseStart;
                        if (shown == 0 || poseMs > 2000)
                            LogF(L"calibrate: pose took %llu ms", poseMs);
                        if (!s->stop.load())
                        {
                            if (++shown == 1)
                                LogF(L"calibrate: first picture after %llu ms", GetTickCount64() - openedMs);
                            {
                                std::lock_guard<std::mutex> lk(g_frameMx);
                                g_latest.bgra.swap(buf);
                                g_latest.w = w;
                                g_latest.h = h;
                                g_latest.pose = pose;
                            }
                            g_lastFrameMs = GetTickCount64();
                            if (!g_framePending.exchange(true))
                                PostMessageW(g_wnd, WM_APP_FRAME, 0, 0);
                        }
                    }
                    mb->Unlock();
                }
                mb->Release();
            }
            sample->Release();
        }

        {
            std::lock_guard<std::mutex> lk(s->mx);
            if (s->source) { s->source->Release(); s->source = nullptr; }
        }
        if (reader) reader->Release();
        if (source) { source->Shutdown(); source->Release(); }
        if (readerAttrs) readerAttrs->Release();
        if (attrs) attrs->Release();
        if (SUCCEEDED(hrCo)) CoUninitialize();
        LogF(L"calibrate: camera closed - %u frames read, %u shown, %u wrong size, %u empty reads",
             frames, shown, wrongSize, empty);
        SetEvent(s->done);
    }

    void StopCamera()
    {
        if (std::shared_ptr<CamSession> s = std::move(g_camSession))
        {
            s->stop = true;
            {
                std::lock_guard<std::mutex> lk(s->mx);
                if (s->source) s->source->Shutdown();   // makes a blocked ReadSample return
            }
            if (WaitForSingleObject(s->done, 3000) == WAIT_OBJECT_0)
                g_camThread.join();
            else
            {
                LogF(L"calibrate: camera thread did not stop within 3 s - leaving it behind");
                g_camThread.detach();
                g_camStuck = true;
            }
        }
        std::lock_guard<std::mutex> lk(g_frameMx);
        g_latest = Frame{};
    }

    void StartCamera(int index)
    {
        StopCamera();
        if (index < 0 || index >= static_cast<int>(g_cameras.size())) return;
        LogF(L"calibrate: opening camera '%s'", g_cameras[static_cast<size_t>(index)].name.c_str());
        g_camSession = std::make_shared<CamSession>();
        g_lastFrameMs = GetTickCount64();   // "no frames" is counted from here
        g_camThread = std::thread(CameraThread, g_camSession, g_cameras[static_cast<size_t>(index)].link);
    }

    // ------------------------------------------------------------ thresholds
    struct Plan
    {
        bool squat = false, hip = false, push = false;
        int st = 0, gd = 0, gu = 0, dn = 0;      // squat knee
        float hipDrop = 0.0f;
        int pu = 0, pgd = 0, pgu = 0, pdn = 0;   // push-up elbow
        std::wstring squatWhy, pushWhy;
    };

    // The photos are held poses, and a held squat is deeper than a rep
    // done at speed — on top of that the detector's median + EMA filters
    // shave the peak off a fast rep. So the "down" thresholds sit well
    // above the photo (45-60% of the range), the "standing" ones just
    // below it (15-25%). First field test: standing 175 / squat 76 gave
    // down=96, and no rep ever got that deep; the hand-tuned config that
    // worked for that child was 158/150/135/125.
    void KeepOrder(int& hi, int& lo, int gap)
    {
        if (hi - lo >= gap) return;
        const int mid = (hi + lo) / 2;
        hi = mid + (gap + 1) / 2;
        lo = hi - gap;
    }

    Plan ComputePlan()
    {
        Plan p;
        const Shot& s1 = g_shots[1];
        const Shot& s2 = g_shots[2];
        if (s1.done && s2.done)
        {
            const float top = s1.angle, bot = s2.angle, range = top - bot;
            if (range >= 30.0f)
            {
                p.squat = true;
                p.st = static_cast<int>(std::lround(top - std::max(6.0f, 0.15f * range)));
                p.gd = static_cast<int>(std::lround(top - std::max(10.0f, 0.25f * range)));
                p.gu = static_cast<int>(std::lround(bot + 0.60f * range));
                p.dn = static_cast<int>(std::lround(bot + 0.45f * range));
                KeepOrder(p.gd, p.gu, 4);
                KeepOrder(p.gu, p.dn, 4);
                if (s1.hipY > 0 && s2.hipY > s1.hipY)
                {
                    // The detector also wants the hips to really go down;
                    // ask for ~30% of the posed drop (reps are shallower
                    // than the held photo, kids stand far from the camera).
                    p.hip = true;
                    p.hipDrop = std::clamp(std::round((s2.hipY - s1.hipY) * 0.3f * 100.0f) / 100.0f, 0.02f, 0.08f);
                }
            }
            else
                p.squatWhy = Fmt(T(tSmallSquat), static_cast<int>(std::lround(range)));
        }
        const Shot& s3 = g_shots[3];
        const Shot& s4 = g_shots[4];
        if (s3.done && s4.done)
        {
            // MoveNet reads elbows in profile with a small span (the
            // hand-tuned config used 158/150/148/142), so accept ranges
            // from 15 degrees.
            const float top = s3.angle, bot = s4.angle, range = top - bot;
            if (range >= 15.0f)
            {
                p.push = true;
                p.pu  = static_cast<int>(std::lround(top - std::max(4.0f, 0.15f * range)));
                p.pgd = static_cast<int>(std::lround(top - std::max(6.0f, 0.30f * range)));
                p.pgu = static_cast<int>(std::lround(bot + 0.55f * range));
                p.pdn = static_cast<int>(std::lround(bot + 0.40f * range));
                KeepOrder(p.pgd, p.pgu, 2);
                KeepOrder(p.pgu, p.pdn, 2);
            }
            else
                p.pushWhy = Fmt(T(tSmallPush), static_cast<int>(std::lround(range)));
        }
        return p;
    }

    std::wstring OldVal(const wchar_t* sec, const wchar_t* key)
    {
        std::wstring v = IniGet(sec, key);
        return v.empty() ? L"?" : v;
    }

    std::wstring SummaryText()
    {
        const Plan p = ComputePlan();
        std::wstring s;
        if (p.squat)
            s += Fmt(T(tSumSquat),
                     OldVal(L"squat", L"standing_angle").c_str(), p.st, OldVal(L"squat", L"going_down_angle").c_str(), p.gd,
                     OldVal(L"squat", L"going_up_angle").c_str(), p.gu, OldVal(L"squat", L"down_angle").c_str(), p.dn);
        else
            s += p.squatWhy.empty() ? Fmt(T(tSumKept), g_uk ? L"Присідання" : L"Squats") : p.squatWhy;
        if (p.hip)
            s += L"\n" + Fmt(T(tSumHip), OldVal(L"squat", L"min_hip_drop").c_str(), p.hipDrop);
        s += L"\n\n";
        if (p.push)
            s += Fmt(T(tSumPush),
                     OldVal(L"pushup", L"up_angle").c_str(), p.pu, OldVal(L"pushup", L"going_down_angle").c_str(), p.pgd,
                     OldVal(L"pushup", L"going_up_angle").c_str(), p.pgu, OldVal(L"pushup", L"down_angle").c_str(), p.pdn);
        else
            s += p.pushWhy.empty() ? Fmt(T(tSumKept), g_uk ? L"Віджимання" : L"Push-ups") : p.pushWhy;
        const int cam = static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0));
        if (cam >= 0 && cam < static_cast<int>(g_cameras.size()))
            s += L"\n\n" + Fmt(T(tSumCamera), g_cameras[static_cast<size_t>(cam)].name.c_str());
        return s;
    }

    bool RestartService()
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return false;
        SC_HANDLE svc = OpenServiceW(scm, L"ExerciseUnlockService", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
        bool ok = false;
        if (svc)
        {
            SERVICE_STATUS ss{};
            if (ControlService(svc, SERVICE_CONTROL_STOP, &ss) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
            {
                for (int i = 0; i < 100; ++i)
                {
                    if (!QueryServiceStatus(svc, &ss) || ss.dwCurrentState == SERVICE_STOPPED) break;
                    Sleep(100);
                }
            }
            ok = StartServiceW(svc, 0, nullptr) != FALSE || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
        return ok;
    }

    bool Save()
    {
        const Plan p = ComputePlan();
        const int cam = static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0));
        const bool setCamera = g_cameras.size() > 1 && cam >= 0;
        if (!p.squat && !p.push && !setCamera)
        {
            g_status = T(tNothingToSave);
            g_statusIsError = true;
            return false;
        }

        CopyFileW(g_configPath.c_str(), (g_configPath + L".bak").c_str(), FALSE);
        bool ok = true;
        auto put = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& val)
        {
            if (!WritePrivateProfileStringW(sec, key, val.c_str(), g_configPath.c_str())) ok = false;
        };
        if (p.squat)
        {
            put(L"squat", L"standing_angle", std::to_wstring(p.st));
            put(L"squat", L"going_down_angle", std::to_wstring(p.gd));
            put(L"squat", L"going_up_angle", std::to_wstring(p.gu));
            put(L"squat", L"down_angle", std::to_wstring(p.dn));
            if (p.hip) put(L"squat", L"min_hip_drop", Fmt(L"%.2f", p.hipDrop));
        }
        if (p.push)
        {
            put(L"pushup", L"up_angle", std::to_wstring(p.pu));
            put(L"pushup", L"going_down_angle", std::to_wstring(p.pgd));
            put(L"pushup", L"going_up_angle", std::to_wstring(p.pgu));
            put(L"pushup", L"down_angle", std::to_wstring(p.pdn));
        }
        // Only pin the camera when there is a choice to make: with a
        // single camera a pinned name would just make the service wait
        // if it ever gets renamed / replaced.
        if (setCamera) put(L"camera", L"preferred", g_cameras[static_cast<size_t>(cam)].name);
        // The angles are measured on x / width, y / height of this camera's
        // frame, so they depend on its aspect ratio. The phone camera page
        // rescales its poses to it (service PhoneLink sends it along).
        for (int i = 1; i <= 4; ++i)
        {
            const Frame& f = g_shots[i].photo;
            if (g_shots[i].done && f.w > 0 && f.h > 0)
            {
                put(L"calibration", L"aspect", Fmt(L"%.4f", static_cast<double>(f.w) / f.h));
                break;
            }
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_configPath.c_str());   // flush

        if (!ok)
        {
            g_status = Fmt(T(tSaveFailed), GetLastError());
            g_statusIsError = true;
            return false;
        }
        LogF(L"calibrate: saved squat=%d (%d/%d/%d/%d hip %.2f) push=%d (%d/%d/%d/%d)",
             p.squat ? 1 : 0, p.st, p.gd, p.gu, p.dn, p.hipDrop, p.push ? 1 : 0, p.pu, p.pgd, p.pgu, p.pdn);
        g_status = T(tSaved);
        if (!g_setupMode) g_status += RestartService() ? T(tRestarted) : T(tRestartFailed);
        g_statusIsError = false;
        return true;
    }

    // ------------------------------------------------------------ capture
    void FinishCapture()
    {
        Shot& shot = g_shots[g_step];
        g_phase = Phase::Live;
        if (g_samples.size() < kSamples / 2)
        {
            g_status = T(g_step <= 2 ? tLegsNotSeen : tPushNotSeen);
            g_statusIsError = true;
            return;
        }
        shot.done = true;
        shot.skipped = false;
        shot.angle = Median(g_samples);
        shot.hipY = Median(g_hipSamples);
        {
            std::lock_guard<std::mutex> lk(g_frameMx);
            shot.photo = g_latest;
        }
        LogF(L"calibrate: step %d angle=%.1f hipY=%.3f samples=%zu", g_step, shot.angle, shot.hipY, g_samples.size());
        g_status.clear();
    }

    void UpdateControls();

    void EndCapture()
    {
        FinishCapture();
        UpdateControls();
    }

    void OnFrame()
    {
        g_framePending = false;
        if (g_status == T(tNoFrames)) g_status.clear();   // the picture came after all
        if (g_phase == Phase::Collecting)
        {
            BodyPose pose;
            {
                std::lock_guard<std::mutex> lk(g_frameMx);
                pose = g_latest.pose;
            }
            const float v = (g_step <= 2) ? KneeAngle(pose) : PushUpElbow(pose);
            if (v > 0.0f)
            {
                g_samples.push_back(v);
                const float hy = HipY(pose);
                if (g_step <= 2 && hy > 0.0f) g_hipSamples.push_back(hy);
            }
            if (g_samples.size() >= kSamples) EndCapture();
        }
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    // ------------------------------------------------------------ UI
    void UpdateControls()
    {
        const bool photoStep = g_step >= 1 && g_step <= 4;
        const bool busy = g_phase != Phase::Live;
        EnableWindow(g_back, g_step > 0 && !busy && !(g_step == kSteps - 1 && g_saved));
        ShowWindow(g_take, photoStep ? SW_SHOW : SW_HIDE);
        ShowWindow(g_skip, photoStep ? SW_SHOW : SW_HIDE);
        EnableWindow(g_take, photoStep && !busy && !g_cameras.empty());
        EnableWindow(g_skip, !busy);
        EnableWindow(g_cam, !busy);
        if (photoStep) SetWindowTextW(g_take, T(g_shots[g_step].done ? tRetake : tTake));
        if (g_step == kSteps - 1) SetWindowTextW(g_next, T(g_saved ? tClose : tSave));
        else SetWindowTextW(g_next, T(tNext));
        EnableWindow(g_next, !busy && (!photoStep || g_shots[g_step].done));
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    void GoToStep(int step)
    {
        g_step = std::clamp(step, 0, kSteps - 1);
        g_phase = Phase::Live;
        g_status.clear();
        UpdateControls();
    }

    RECT VideoRect(const RECT& client)
    {
        return RECT{ Px(16), Px(16), client.right - Px(440), client.bottom - Px(16) };
    }

    // Wide enough for four buttons with Ukrainian captions ("Зробити фото").
    RECT PanelRect(const RECT& client)
    {
        return RECT{ client.right - Px(424), Px(16), client.right - Px(16), client.bottom - Px(16) };
    }

    void Layout()
    {
        RECT rc;
        GetClientRect(g_wnd, &rc);
        const RECT p = PanelRect(rc);
        const int w = p.right - p.left;
        MoveWindow(g_cam, p.left, p.top + Px(22), w, Px(300), TRUE);
        const int y = p.bottom - Px(32);
        const int bw = (w - Px(24)) / 4;
        MoveWindow(g_back, p.left, y, bw, Px(32), TRUE);
        MoveWindow(g_skip, p.left + bw + Px(8), y, bw, Px(32), TRUE);
        MoveWindow(g_take, p.left + 2 * (bw + Px(8)), y, bw, Px(32), TRUE);
        MoveWindow(g_next, p.left + 3 * (bw + Px(8)), y, bw, Px(32), TRUE);
    }

    void MakeFonts()
    {
        for (HFONT f : { g_fontUi, g_fontTitle }) if (f) DeleteObject(f);
        auto mk = [](int pt, int weight) {
            return CreateFontW(-MulDiv(pt, static_cast<int>(g_dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        };
        g_fontUi = mk(10, FW_NORMAL);
        g_fontTitle = mk(15, FW_SEMIBOLD);
        for (HWND h : { g_cam, g_back, g_take, g_skip, g_next })
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUi), TRUE);
    }

    void DrawSkeleton(HDC dc, const RECT& r, const BodyPose& pose)
    {
        static const int bones[][2] = {
            { 5, 7 }, { 7, 9 }, { 6, 8 }, { 8, 10 }, { 5, 6 }, { 5, 11 }, { 6, 12 }, { 11, 12 },
            { 11, 13 }, { 13, 15 }, { 12, 14 }, { 14, 16 }, { 0, 5 }, { 0, 6 } };
        const float W = static_cast<float>(r.right - r.left), H = static_cast<float>(r.bottom - r.top);
        auto pt = [&](int i) {
            return POINT{ r.left + static_cast<LONG>(pose.joints[i].x * W), r.top + static_cast<LONG>(pose.joints[i].y * H) };
        };
        HPEN pen = CreatePen(PS_SOLID, Px(3), RGB(43, 255, 136));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        for (const auto& b : bones)
        {
            if (pose.joints[b[0]].confidence < 0.2f || pose.joints[b[1]].confidence < 0.2f) continue;
            const POINT a = pt(b[0]), c = pt(b[1]);
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, c.x, c.y);
        }
        HBRUSH dot = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ oldBrush = SelectObject(dc, dot);
        for (int i = 0; i < BodyPose::kJointCount; ++i)
        {
            if (pose.joints[i].confidence < 0.2f) continue;
            const POINT c = pt(i);
            const int rad = Px(5);
            Ellipse(dc, c.x - rad, c.y - rad, c.x + rad, c.y + rad);
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(dot);
        DeleteObject(pen);
    }

    void DrawShadowText(HDC dc, const std::wstring& s, RECT r, UINT fmt, COLORREF color)
    {
        SetBkMode(dc, TRANSPARENT);
        RECT sh = r;
        OffsetRect(&sh, Px(2), Px(2));
        SetTextColor(dc, RGB(0, 0, 0));
        DrawTextW(dc, s.c_str(), -1, &sh, fmt);
        SetTextColor(dc, color);
        DrawTextW(dc, s.c_str(), -1, &r, fmt);
    }

    void PaintVideo(HDC dc, const RECT& area)
    {
        HBRUSH bg = CreateSolidBrush(RGB(30, 30, 40));
        FillRect(dc, &area, bg);
        DeleteObject(bg);

        // Photo steps show the taken photo until "Retake"; otherwise live.
        Frame f;
        const bool showPhoto = g_step >= 1 && g_step <= 4 && g_shots[g_step].done && g_phase == Phase::Live;
        if (showPhoto) f = g_shots[g_step].photo;
        else { std::lock_guard<std::mutex> lk(g_frameMx); f = g_latest; }

        SelectObject(dc, g_fontUi);
        if (f.bgra.empty())
        {
            DrawShadowText(dc, g_cameras.empty() ? T(tNoCamera) : L"...", area,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(220, 220, 230));
            return;
        }
        const int aw = area.right - area.left, ah = area.bottom - area.top;
        const double scale = std::min(static_cast<double>(aw) / f.w, static_cast<double>(ah) / f.h);
        const int vw = static_cast<int>(f.w * scale), vh = static_cast<int>(f.h * scale);
        const RECT v{ area.left + (aw - vw) / 2, area.top + (ah - vh) / 2, area.left + (aw - vw) / 2 + vw, area.top + (ah - vh) / 2 + vh };

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = static_cast<LONG>(f.w);
        bi.bmiHeader.biHeight = -static_cast<LONG>(f.h);   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, v.left, v.top, vw, vh, 0, 0, static_cast<int>(f.w), static_cast<int>(f.h),
                      f.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        DrawSkeleton(dc, v, f.pose);

        // Live measurement badge for the current step.
        if (g_step >= 1 && g_step <= 4)
        {
            const float a = (g_step <= 2) ? KneeAngle(f.pose) : PushUpElbow(f.pose);
            std::wstring label = (a > 0) ? Fmt(T(g_step <= 2 ? tKnee : tElbow), static_cast<int>(std::lround(a))) : std::wstring(L"?");
            if (showPhoto) label = Fmt(T(g_step <= 2 ? tKnee : tElbow), static_cast<int>(std::lround(g_shots[g_step].angle)));
            RECT lr{ v.left + Px(12), v.top + Px(8), v.right, v.top + Px(40) };
            SelectObject(dc, g_fontTitle);
            DrawShadowText(dc, label, lr, DT_LEFT | DT_SINGLELINE, RGB(255, 230, 90));
        }
    }

    // ------------------------------------------------------------ example pictures
    // A side-view figure per step: step 0 shows where the camera goes,
    // steps 1-4 what the camera should see. Figure units: y grows down,
    // the floor is at y = 100, the figure faces right.
    struct FigPt { float x, y; };
    struct Figure
    {
        FigPt head; float headR;
        FigPt neck, shoulder, elbow, wrist, hip, knee, ankle, toe;
        int ring;   // joint the step measures: 1 knee, 2 elbow
    };

    const Figure kFigures[4] = {
        // standing: legs straight, arms down
        { { 51, 15 }, 6.5f, { 50, 23 }, { 50, 27 }, { 52, 41 }, { 55, 54 }, { 50, 55 }, { 51, 76 }, { 50, 96 }, { 57, 97 }, 1 },
        // squat: thighs about level, arms forward
        { { 57, 40 }, 6.5f, { 54, 46 }, { 52, 50 }, { 65, 53 }, { 78, 52 }, { 40, 74 }, { 60, 77 }, { 50, 96 }, { 57, 97 }, 1 },
        // push-up, arms straight: hands under the shoulders, body in one line
        { { 88, 66 }, 6.5f, { 82, 70 }, { 78, 72 }, { 78, 85 }, { 78, 98 }, { 52, 80 }, { 33, 86 }, { 14, 92 }, { 11, 98 }, 2 },
        // push-up, bottom: chest low, elbows bent about 90 degrees and
        // sticking up above the back
        { { 96, 85 }, 6.5f, { 90, 88.5f }, { 86, 89.5f }, { 73, 84 }, { 71, 98 }, { 59, 91.5f }, { 39, 93 }, { 19, 94.5f }, { 16, 98.5f }, 2 },
    };

    // Scene extent in figure units (figure, plus camera and distance label on step 0).
    struct Bounds { float x0, x1, y0, y1; };
    Bounds ExampleBounds(int step)
    {
        const bool scene = step == 0;
        const Figure& f = kFigures[scene ? 0 : step - 1];
        const float dx = scene ? 62.0f : 0.0f;
        Bounds b{ 1e9f, -1e9f, 1e9f, 0 };
        auto grow = [&](FigPt pt, float r) {
            b.x0 = std::min(b.x0, pt.x + dx - r);
            b.x1 = std::max(b.x1, pt.x + dx + r);
            b.y0 = std::min(b.y0, pt.y - r);
        };
        for (FigPt pt : { f.neck, f.shoulder, f.elbow, f.wrist, f.hip, f.knee, f.ankle, f.toe }) grow(pt, 0);
        grow(f.head, f.headR);
        if (scene) b.x0 = 2.0f;   // camera on its tripod
        // Both push-up pictures at the same scale, so they compare at a glance.
        if (step >= 3) b.y0 = std::min(b.y0, kFigures[2].head.y - kFigures[2].headR);
        b.y1 = scene ? 118.0f : 104.0f;
        return b;
    }
    constexpr float kExamplePad = 9.0f;

    // Card height that fits the scene at full card width.
    int ExampleHeight(int step, int width)
    {
        const Bounds b = ExampleBounds(step);
        const float inset = static_cast<float>(Px(8));
        const float sceneW = b.x1 - b.x0 + 2 * kExamplePad, sceneH = b.y1 - (b.y0 - kExamplePad);
        return static_cast<int>((static_cast<float>(width) - 2 * inset) * sceneH / sceneW + 2 * inset);
    }

    void RoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r)
    {
        path.AddArc(x, y, 2 * r, 2 * r, 180, 90);
        path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
        path.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
        path.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
        path.CloseFigure();
    }

    void DrawExample(HDC dc, const RECT& card, int step)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        Gdiplus::GraphicsPath bgPath;
        RoundedRect(bgPath, static_cast<float>(card.left), static_cast<float>(card.top),
                    static_cast<float>(card.right - card.left), static_cast<float>(card.bottom - card.top), static_cast<float>(Px(10)));
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 241, 244, 251));
        g.FillPath(&bgBrush, &bgPath);

        const bool scene = step == 0;
        const Figure& f = kFigures[scene ? 0 : step - 1];
        const float figDx = scene ? 62.0f : 0.0f;   // step 0: the child stands right of the camera

        // Fit the scene into the card.
        const Bounds b = ExampleBounds(step);
        const float x0 = b.x0, x1 = b.x1, y0 = b.y0;
        const float sx0 = x0 - kExamplePad, sx1 = x1 + kExamplePad, sy0 = y0 - kExamplePad, sy1 = b.y1;
        const float inset = static_cast<float>(Px(8));
        const float bw = card.right - card.left - 2 * inset, bh = card.bottom - card.top - 2 * inset;
        const float u = std::min(bw / (sx1 - sx0), bh / (sy1 - sy0));
        const float ox = card.left + inset + (bw - (sx1 - sx0) * u) / 2 - sx0 * u;
        const float oy = card.top + inset + (bh - (sy1 - sy0) * u) / 2 - sy0 * u;
        auto S = [&](float x, float y) { return Gdiplus::PointF(ox + x * u, oy + y * u); };
        auto F = [&](FigPt pt, float dx = 0, float dy = 0) { return S(pt.x + figDx + dx, pt.y + dy); };

        const Gdiplus::Color ink(255, 52, 64, 110), farInk(255, 163, 172, 203), accent(255, 60, 110, 220);
        auto roundPen = [&](Gdiplus::Pen& pen) {
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
        };

        Gdiplus::Pen floor(Gdiplus::Color(255, 196, 202, 222), 1.4f * u);
        g.DrawLine(&floor, S(sx0 + 3, 100.5f), S(sx1 - 3, 100.5f));

        if (scene)
        {
            // Camera on a low tripod, lens towards the child, view covering head to feet.
            Gdiplus::Pen tripod(farInk, 1.6f * u);
            roundPen(tripod);
            g.DrawLine(&tripod, S(11, 60), S(5, 100));
            g.DrawLine(&tripod, S(11, 60), S(17, 100));
            g.DrawLine(&tripod, S(11, 60), S(11, 100));
            Gdiplus::GraphicsPath body;
            RoundedRect(body, S(4, 50).X, S(4, 50).Y, 15 * u, 10 * u, 2 * u);
            Gdiplus::SolidBrush inkBrush(ink);
            g.FillPath(&inkBrush, &body);
            g.FillEllipse(&inkBrush, S(17, 51.5f).X, S(17, 51.5f).Y, 7 * u, 7 * u);
            Gdiplus::Pen view(accent, 1.0f * u);
            view.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawLine(&view, S(24, 55), S(sx1 - 3, y0 - 4));
            g.DrawLine(&view, S(24, 55), S(sx1 - 3, 100));
            // Distance arrow along the floor with "2-3 m".
            Gdiplus::Pen arrow(ink, 1.0f * u);
            Gdiplus::AdjustableArrowCap cap(3.0f, 3.0f, TRUE);
            arrow.SetCustomStartCap(&cap);
            arrow.SetCustomEndCap(&cap);
            g.DrawLine(&arrow, S(13, 106), S(f.ankle.x + figDx, 106));
            Gdiplus::FontFamily family(L"Segoe UI");
            Gdiplus::Font font(&family, 7.5f * u, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::StringFormat center;
            center.SetAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(T(tDistance), -1, &font, S((13 + f.ankle.x + figDx) / 2, 107), &center, &inkBrush);
        }
        else
        {
            // Viewfinder corners: the whole body has to be in the frame.
            Gdiplus::Pen corner(accent, 1.4f * u);
            roundPen(corner);
            const float cx0 = x0 - 5, cx1 = x1 + 5, cy0 = y0 - 5, cy1 = 102.0f, l = 7.0f;
            g.DrawLine(&corner, S(cx0, cy0 + l), S(cx0, cy0)); g.DrawLine(&corner, S(cx0, cy0), S(cx0 + l, cy0));
            g.DrawLine(&corner, S(cx1 - l, cy0), S(cx1, cy0)); g.DrawLine(&corner, S(cx1, cy0), S(cx1, cy0 + l));
            g.DrawLine(&corner, S(cx0, cy1 - l), S(cx0, cy1)); g.DrawLine(&corner, S(cx0, cy1), S(cx0 + l, cy1));
            g.DrawLine(&corner, S(cx1 - l, cy1), S(cx1, cy1)); g.DrawLine(&corner, S(cx1, cy1), S(cx1, cy1 - l));
        }

        // The figure: far arm and leg lighter and a little behind, then torso, near limbs, head.
        Gdiplus::Pen farPen(farInk, 3.6f * u), nearPen(ink, 3.6f * u), torsoPen(ink, 5.6f * u);
        roundPen(farPen); roundPen(nearPen); roundPen(torsoPen);
        auto limb = [&](Gdiplus::Pen& pen, std::initializer_list<FigPt> pts, float dx, float dy) {
            std::vector<Gdiplus::PointF> v;
            for (FigPt pt : pts) v.push_back(F(pt, dx, dy));
            g.DrawLines(&pen, v.data(), static_cast<INT>(v.size()));
        };
        const bool lying = !scene && step >= 3;
        const float fdx = lying ? -4.0f : -3.0f, fdy = lying ? 0.0f : -1.5f;
        // Soft highlight behind the joint whose angle this photo measures.
        const float ringR = 7.5f * u;
        const Gdiplus::PointF joint = F(f.ring == 1 ? f.knee : f.elbow);
        if (!scene)
        {
            Gdiplus::SolidBrush halo(Gdiplus::Color(70, 255, 140, 40));
            g.FillEllipse(&halo, joint.X - ringR, joint.Y - ringR, 2 * ringR, 2 * ringR);
        }
        limb(farPen, { f.shoulder, f.elbow, f.wrist }, fdx, fdy);
        limb(farPen, { f.hip, f.knee, f.ankle, f.toe }, fdx, fdy);
        limb(torsoPen, { f.hip, f.shoulder, f.neck }, 0, 0);
        limb(nearPen, { f.hip, f.knee, f.ankle, f.toe }, 0, 0);
        limb(nearPen, { f.shoulder, f.elbow, f.wrist }, 0, 0);
        Gdiplus::SolidBrush headBrush(ink);
        const Gdiplus::PointF hc = F(f.head);
        g.FillEllipse(&headBrush, hc.X - f.headR * u, hc.Y - f.headR * u, 2 * f.headR * u, 2 * f.headR * u);
        // Eye on the facing side, so the profile reads at a glance.
        Gdiplus::SolidBrush eyeBrush(Gdiplus::Color(255, 255, 255, 255));
        const float eye = 1.3f * u;
        const Gdiplus::PointF ec = F(f.head, 3.0f, lying ? 0.5f : -1.0f);
        g.FillEllipse(&eyeBrush, ec.X - eye, ec.Y - eye, 2 * eye, 2 * eye);

        if (!scene)
        {
            // Thin ring around it, wide enough to leave the limbs visible.
            Gdiplus::Pen ring(Gdiplus::Color(255, 255, 140, 40), 1.1f * u);
            g.DrawEllipse(&ring, joint.X - ringR, joint.Y - ringR, 2 * ringR, 2 * ringR);
        }
    }

    void PaintPanel(HDC dc, const RECT& p)
    {
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        RECT full{ p.left - Px(8), 0, p.right + Px(16), p.bottom + Px(16) };
        FillRect(dc, &full, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        SelectObject(dc, g_fontUi);
        SetTextColor(dc, RGB(90, 96, 130));
        RECT r{ p.left, p.top, p.right, p.top + Px(20) };
        DrawTextW(dc, T(tCamera), -1, &r, DT_LEFT | DT_SINGLELINE);

        int y = p.top + Px(64);
        r = RECT{ p.left, y, p.right, y + Px(20) };
        DrawTextW(dc, Fmt(T(tStepOf), g_step + 1, kSteps).c_str(), -1, &r, DT_LEFT | DT_SINGLELINE);
        y += Px(22);

        SelectObject(dc, g_fontTitle);
        SetTextColor(dc, RGB(31, 37, 71));
        r = RECT{ p.left, y, p.right, y + Px(40) };
        DrawTextW(dc, T(static_cast<Txt>(tTitle0 + 2 * g_step)), -1, &r, DT_LEFT | DT_WORDBREAK);
        y += Px(40);

        SelectObject(dc, g_fontUi);
        std::wstring body = T(static_cast<Txt>(tText0 + 2 * g_step));
        if (g_step == kSteps - 1) body += L"\n\n" + SummaryText();
        r = RECT{ p.left, y, p.right, p.bottom - Px(150) };
        RECT measure = r;
        DrawTextW(dc, body.c_str(), -1, &measure, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
        DrawTextW(dc, body.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK);
        y = std::min(measure.bottom, r.bottom) + Px(12);

        if (!g_status.empty())
        {
            SetTextColor(dc, g_statusIsError ? RGB(200, 40, 60) : RGB(20, 150, 110));
            r = RECT{ p.left, y, p.right, p.bottom - Px(150) };
            measure = r;
            DrawTextW(dc, g_status.c_str(), -1, &measure, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
            DrawTextW(dc, g_status.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK);
            y = std::min(measure.bottom, r.bottom) + Px(12);
        }

        // Example picture in whatever room is left above the checklist.
        const int exampleBottom = p.bottom - Px(150);
        if (g_step <= 4 && exampleBottom - y >= Px(120))
        {
            SetTextColor(dc, RGB(90, 96, 130));
            r = RECT{ p.left, y, p.right, y + Px(20) };
            DrawTextW(dc, T(g_step == 0 ? tExampleSetup : tExampleView), -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            const int want = std::min(ExampleHeight(g_step, p.right - p.left), Px(240));
            const RECT card{ p.left, y + Px(24), p.right, std::min(exampleBottom, y + Px(24) + want) };
            DrawExample(dc, card, g_step);
        }

        // Photo checklist above the buttons.
        SetTextColor(dc, RGB(31, 37, 71));
        y = p.bottom - Px(140);
        for (int i = 1; i <= 4; ++i)
        {
            const Shot& s = g_shots[i];
            std::wstring v = s.done ? Fmt(T(i <= 2 ? tKnee : tElbow), static_cast<int>(std::lround(s.angle)))
                                    : (s.skipped ? T(tSkipped) : T(tNotYet));
            std::wstring line = std::wstring(s.done ? L"\x2714  " : L"\x2022  ") + T(static_cast<Txt>(tRow1 + i - 1)) + L": " + v;
            r = RECT{ p.left, y, p.right, y + Px(24) };
            DrawTextW(dc, line.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += Px(24);
        }
    }

    void OnPaint()
    {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(g_wnd, &ps);
        RECT rc;
        GetClientRect(g_wnd, &rc);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(244, 246, 252));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        PaintVideo(dc, VideoRect(rc));
        PaintPanel(dc, PanelRect(rc));
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(g_wnd, &ps);
    }

    void OnCommand(int id, int code)
    {
        switch (id)
        {
        case idCamera:
            if (code == CBN_SELCHANGE)
            {
                g_status.clear();
                StartCamera(static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0)));
                InvalidateRect(g_wnd, nullptr, FALSE);
            }
            break;
        case idBack:
            GoToStep(g_step - 1);
            break;
        case idTake:
            g_status.clear();
            g_samples.clear();
            g_hipSamples.clear();
            g_shots[g_step].done = false;
            g_phase = Phase::Collecting;   // the next ~12 frames give the angle (median)
            g_phaseStart = GetTickCount64();
            UpdateControls();
            break;
        case idSkip:
            g_shots[g_step].done = false;
            g_shots[g_step].skipped = true;
            GoToStep(g_step + 1);
            break;
        case idNext:
            if (g_step < kSteps - 1) GoToStep(g_step + 1);
            else if (g_saved) DestroyWindow(g_wnd);
            else { g_saved = Save(); UpdateControls(); }
            break;
        default:
            break;
        }
    }

    void OnTimer()
    {
        if (g_phase == Phase::Collecting && GetTickCount64() - g_phaseStart >= kCollectMs)
        {
            EndCapture();
        }
        // An open camera that stays silent: say so instead of an endless "...".
        if (g_camSession && g_status.empty() && GetTickCount64() - g_lastFrameMs.load() >= kNoFramesMs)
        {
            LogF(L"calibrate: no picture for %llu s", kNoFramesMs / 1000);
            g_status = T(tNoFrames);
            g_statusIsError = true;
        }
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            g_wnd = hwnd;
            g_dpi = GetDpiForWindow(hwnd);
            g_cam = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                    0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(idCamera)), g_inst, nullptr);
            auto button = [&](int id) {
                return CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
            };
            g_back = button(idBack);
            g_skip = button(idSkip);
            g_take = button(idTake);
            g_next = button(idNext);
            SetWindowTextW(g_back, T(tBack));
            SetWindowTextW(g_skip, T(tSkip));
            MakeFonts();

            g_cameras = EnumerateCameras();
            int pick = 0;
            const std::wstring pref = IniGet(L"camera", L"preferred");
            for (size_t i = 0; i < g_cameras.size(); ++i)
            {
                SendMessageW(g_cam, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_cameras[i].name.c_str()));
                if (!pref.empty() && StrStrIW(g_cameras[i].name.c_str(), pref.c_str())) pick = static_cast<int>(i);
            }
            if (!g_cameras.empty())
            {
                SendMessageW(g_cam, CB_SETCURSEL, static_cast<WPARAM>(pick), 0);
                if (g_snapshotPath.empty()) StartCamera(pick);
            }
            if (!g_poseOk) { g_status = T(tNoModel); g_statusIsError = true; }
            Layout();
            if (!g_snapshotPath.empty())
            {
                // Layout check without a camera: sample measurements.
                const float sample[5] = { 0.0f, 171.0f, 96.0f, 164.0f, 108.0f };
                for (int i = 1; i <= 4 && i < g_snapshotStep + (g_snapshotStep == kSteps - 1 ? 1 : 0); ++i)
                {
                    g_shots[i].done = true;
                    g_shots[i].angle = sample[i];
                    g_shots[i].hipY = (i == 1) ? 0.50f : 0.62f;
                }
                GoToStep(g_snapshotStep);
                SetTimer(hwnd, kSnapshotTimerId, 1500, nullptr);   // let DWM compose the window first
            }
            else
                GoToStep(0);
            SetTimer(hwnd, kTimerId, 100, nullptr);
            return 0;
        }
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = POINT{ Px(900), Px(600) };
            return 0;
        }
        case WM_DPICHANGED:
        {
            g_dpi = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            MakeFonts();
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_TIMER:
            if (wp == kSnapshotTimerId)
            {
                KillTimer(hwnd, kSnapshotTimerId);
                SendMessageW(hwnd, WM_APP_SNAPSHOT, 0, 0);
                return 0;
            }
            OnTimer();
            return 0;
        case WM_APP_FRAME:
            OnFrame();
            return 0;
        case WM_APP_SNAPSHOT:
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC wdc = GetDC(hwnd);
            HDC dc = CreateCompatibleDC(wdc);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = rc.right;
            bi.bmiHeader.biHeight = rc.bottom;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(dc, bmp);
            PrintWindow(hwnd, dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
            GdiFlush();
            BITMAPFILEHEADER fh{};
            const DWORD imageBytes = static_cast<DWORD>(rc.right) * static_cast<DWORD>(rc.bottom) * 4;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
            fh.bfSize = fh.bfOffBits + imageBytes;
            HANDLE f = CreateFileW(g_snapshotPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD wrote = 0;
                WriteFile(f, &fh, sizeof(fh), &wrote, nullptr);
                WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wrote, nullptr);
                WriteFile(f, bits, imageBytes, &wrote, nullptr);
                CloseHandle(f);
            }
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
            ReleaseDC(hwnd, wdc);
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_APP_CAMERR:
            g_status = T(tCamBusy);
            g_statusIsError = true;
            g_phase = Phase::Live;
            UpdateControls();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            StopCamera();
            PostQuitMessage(g_saved ? 0 : 1);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_uk = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_UKRAINIAN;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"/setup") == 0) g_setupMode = true;
            else if (_wcsicmp(argv[i], L"/lang") == 0 && i + 1 < argc) g_uk = _wcsicmp(argv[++i], L"uk") == 0;
            else if (_wcsicmp(argv[i], L"/snapshot") == 0 && i + 1 < argc) g_snapshotPath = argv[++i];
            else if (_wcsicmp(argv[i], L"/step") == 0 && i + 1 < argc) g_snapshotStep = _wtoi(argv[++i]);
        }
        LocalFree(argv);
    }

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    g_exeDir = exe;
    g_configPath = g_exeDir + L"\\config.ini";

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);   // example pictures
    g_poseOk = SUCCEEDED(g_pose.Load(g_exeDir + L"\\movenet_lightning.onnx"));
    LogF(L"calibrate: start lang=%s setup=%d pose=%d", g_uk ? L"uk" : L"en", g_setupMode ? 1 : 0, g_poseOk ? 1 : 0);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ExerciseUnlockCalibrate";
    wc.hIcon = static_cast<HICON>(LoadImageW(nullptr, (g_exeDir + L"\\app.ico").c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    RegisterClassExW(&wc);

    g_dpi = GetDpiForSystem();
    const bool snapshot = !g_snapshotPath.empty();
    HWND hwnd = CreateWindowExW(snapshot ? WS_EX_TOOLWINDOW : 0, wc.lpszClassName, T(tWindow), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                snapshot ? -6000 : CW_USEDEFAULT, snapshot ? -6000 : CW_USEDEFAULT,
                                Px(1200), Px(760), nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 2;
    if (snapshot)
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    else
    {
        ShowWindow(hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
        SetForegroundWindow(hwnd);
    }

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    const int rc = static_cast<int>(m.wParam);   // 0 = saved, 1 = closed without saving
    // A camera thread still stuck in Media Foundation could hang the
    // orderly shutdown below - and setup waits for this process.
    if (g_camStuck) TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    Gdiplus::GdiplusShutdown(gdipToken);
    MFShutdown();
    CoUninitialize();
    return rc;
}
