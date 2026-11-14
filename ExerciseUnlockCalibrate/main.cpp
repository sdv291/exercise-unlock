// ExerciseUnlockCalibrate — the child does 5 reps of each exercise (squats,
// push-ups, sit-ups) in front of the webcam or the phone. The turning points
// of the angle the service's own detectors see in them (standing / squatting,
// arms straight / the bottom of the push-up, lying / sitting up) give the
// rep-detection thresholds, written into config.ini (CheckJudge.h).
//
//   ExerciseUnlockCalibrate.exe [/lang en|uk] [/setup] [/settings] [/phone]
//   (no /lang: the language setup ran in, HKLM\SOFTWARE\ExerciseUnlock\Language)
//
//   /setup     after saving, don't restart the service (a run before it is
//              installed or started).
//   /settings  only the settings window (Settings.cpp), no wizard and no
//              camera.
//   /phone     start with "Phone (QR code)" picked: no webcam is opened.
//
// "Phone (QR code)" in the camera list calibrates the phone camera instead:
// the phone page streams its poses through the relay, the same steps run on
// them, and the result goes into the phone's own sections ([phone_squat]
// ...) - the webcam's stay as they are. The phone shows each step and the
// wizard's buttons, so the whole calibration can be done from it.
//
// Angles are measured exactly the way SquatDetector / PushUpDetector see
// them: same MoveNet model (PoseEstimator.cpp), same camera format (RGB32
// at the camera's default size — angles are taken in normalized frame
// coordinates, so the aspect ratio must match the service's), same joints,
// same confidence gates, same "is this a push-up at all" checks.

#define NOMINMAX
#include "PoseEstimator.h"
#include "Config.h"
#include "State.h"
#include "SquatDetector.h"
#include "PushUpDetector.h"
#include "SitUpDetector.h"
#include "PostureClassifier.h"
#include "CheckJudge.h"
#include "Log.h"
#include "Paths.h"
#include "Settings.h"
#include "Snapshot.h"
#include "MiniJson.h"
#include "PhoneCounter.h"
#include "QrCode.h"
#include "RelayClient.h"
#include "Version.h"

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
#include <memory>
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
using ExerciseUnlock::Svc::Config;
using ExerciseUnlock::Svc::Joint;
using ExerciseUnlock::Svc::LogF;
using ExerciseUnlock::Svc::PoseEstimator;
using ExerciseUnlock::Svc::PostureClassifier;
using ExerciseUnlock::Svc::PushUpDetector;
using ExerciseUnlock::Svc::SitUpDetector;
using ExerciseUnlock::Svc::SquatDetector;
using ExerciseUnlock::Calib::Swing;
namespace Ipc = ExerciseUnlock::Ipc;
namespace Json = ExerciseUnlock::Svc::Json;
namespace Relay = ExerciseUnlock::Svc::Relay;
namespace Qr = ExerciseUnlock::Qr;

namespace
{
    // ------------------------------------------------------------ text
    enum Txt
    {
        tWindow, tCamera, tBack, tNext, tSkip, tSave, tClose, tStepOf, tTitle0, tText0, tTitle5, tText5, tKnee,
        tElbow, tSkipped, tNotYet, tNoCamera, tCamBusy, tNoFrames, tNoModel, tSaved, tRestarted, tRestartFailed,
        tSaveFailed, tNothingToSave, tSumSquat, tSumPush, tSumHip, tSumKept, tSumCamera, tExampleSetup, tExampleView,
        tDistance, tRestart, tTestThrSquat, tTestThrPush, tTestNoSquat, tTestNoPlank, tHip, tSumSitup, tTestThrSitup,
        tTestNoSitup, tSettings, tSettingsSaved, tPhone, tPhoneScan, tPhoneServer, tPhoneNoServer, tPhoneWaiting,
        tPhoneReset, tSumPhone, tPhoneStep0, tTitleRepsSquat, tTextRepsSquat, tTitleRepsPush, tTextRepsPush,
        tTitleRepsSit, tTextRepsSit, tRepsCount, tRepsDone, tRepsNext, tRepsShallow, tRepsRange, tRepsHips, tSumReps, tCount
    };

    const wchar_t* const kEn[] = {
        L"Exercise Unlock - calibration",
        L"Camera:",
        L"Back",
        L"Next",
        L"Skip",
        L"Save",
        L"Close",
        L"Step %d of %d",
        L"Camera",
        L"Put the camera where the user will exercise and pick it in the list above.\n\n"
        L"The user must be fully visible from the side - from head to feet. For push-ups and sit-ups the camera has to stand on the floor.\n\n"
        L"Next the user does 5 reps of each exercise at the usual pace: squats, push-ups and sit-ups. Exercise Unlock takes the top and the bottom position from them and tunes the rep counting to them.\n\n"
        L"The seconds per rep, the schedule and everything else are under \"Settings\".",
        L"Result",
        L"These thresholds will be written to config.ini. Skipped exercises keep their current settings.",
        L"knee %d\x00B0",
        L"elbow %d\x00B0",
        L"skipped",
        L"-",
        L"No camera found. Connect a camera and start the calibration again.",
        L"The camera is busy or unavailable. Close other apps that use it and pick the camera again.",
        L"The camera is not sending a picture. Pick another camera in the list above, or close other apps that use it.",
        L"The pose model could not be loaded - is the installation complete?",
        L"Saved to config.ini.",
        L" The service was restarted.",
        L" Restart the service (or the computer) to apply.",
        L"Could not write config.ini (error %lu). Run the calibration as administrator.",
        L"Nothing to save - all exercises were skipped.",
        L"Squats (knee angle)\n  standing %s \x2192 %d, going down %s \x2192 %d\n  going up %s \x2192 %d, down %s \x2192 %d",
        L"Push-ups (elbow angle)\n  up %s \x2192 %d, going down %s \x2192 %d\n  going up %s \x2192 %d, down %s \x2192 %d",
        L"Minimum hip drop %s \x2192 %.2f",
        L"%s: kept as is",
        L"Camera: %s",
        L"Example - where to put the camera:",
        L"Example - what the camera should see:",
        L"2-3 m",
        L"Start over",
        L"A rep counts when the knee bends to %d\x00B0 or less and straightens back to %d\x00B0 or more.",
        L"A rep counts when the elbow bends to %d\x00B0 or less and straightens back to %d\x00B0 or more.",
        L"No squat position recognized: the user must be fully visible, from head to feet.",
        L"No push-up position recognized: the whole body must be visible, hands on the floor.",
        L"hip %d\x00B0",
        L"Sit-ups (hip angle)\n  lying %s \x2192 %d, rising %s \x2192 %d\n  lowering %s \x2192 %d, top %s \x2192 %d",
        L"A rep counts when the hip angle closes to %d\x00B0 or less and opens back to %d\x00B0 or more.",
        L"No sit-up position recognized: the user lies on the back sideways to the camera, knees bent, the whole body visible.",
        L"Settings",
        L"Settings saved.",
        L"Phone (QR code)",
        L"Scan this code with the phone camera and press \"Start\" on the phone. Then put the phone where it will stand "
        L"during the exercises - on the floor 2-3 m away, the user fully visible from the side. The address below can also be typed in.",
        L"Connecting to the server...",
        L"No connection to the server %s - the phone needs the internet on this computer. Pick a webcam or check the connection, "
        L"then pick the phone again.",
        L"The phone is connected - waiting for its camera. Press \"Start\" on the phone.",
        L"The phone and the webcam are calibrated separately: the reps so far were reset.",
        L"Saved as the phone's own settings ([phone_squat], [phone_pushup], [phone_situp] in config.ini); the webcam's stay as they are.",
        L"The phone is connected. Put it where it will stand during the exercises: on the floor, 2-3 m away, side-on to the user, "
        L"so the whole body from head to feet is in the picture.\n\n"
        L"Then press \"Next\": 5 reps of each exercise at the usual pace, and Exercise Unlock takes the positions from them by itself. "
        L"Everything can be done from this phone.",
        L"Squats",
        L"The user stands sideways to the camera, the whole body in the picture, and does 5 squats at the usual pace - the way they will squat every day. Exercise Unlock takes the highest and the lowest position from them by itself.\n\n"
        L"Squat for real, thighs about level with the floor: squats like these are what will count.",
        L"Push-ups",
        L"The camera stands on the floor. Sideways to it the user does 5 push-ups at the usual pace: arms all the way straight, chest down to the floor. Exercise Unlock takes the top and the bottom from them by itself.",
        L"Sit-ups",
        L"The camera stands on the floor. The user lies on the back sideways to it, knees bent, and does 5 sit-ups at the usual pace. Exercise Unlock takes the lying and the top position from them by itself.",
        L"%d of %d reps done",
        L"Done: up %s, down %s.",
        L"On to the next step...",
        L"The squats were too shallow: the knee bent only to %d\x00B0, and it has to reach %d\x00B0 at least. "
        L"Squat deeper and do 5 squats again.",
        L"Too little movement: only %d\x00B0 between the top and the bottom position. Do 5 reps again, all the way, "
        L"sideways to the camera.",
        L"The hips hardly go down in the picture: the camera probably sees the user at a poor angle. Put it side-on to the "
        L"user and do 5 squats again.",
        L"%s: up %s, down %s",
    };

    const wchar_t* const kUk[] = {
        L"Exercise Unlock - калібрування",
        L"Камера:",
        L"Назад",
        L"Далі",
        L"Пропустити",
        L"Зберегти",
        L"Закрити",
        L"Крок %d з %d",
        L"Камера",
        L"Поставте камеру там, де користувач виконуватиме вправи, і виберіть її у списку вище.\n\n"
        L"Користувача має бути видно збоку повністю - від голови до ніг. Для віджимань і преса камера має стояти на підлозі.\n\n"
        L"Далі користувач робить по 5 повторів кожної вправи у звичному темпі: присідання, віджимання й прес. Exercise Unlock сам візьме з них верхнє й нижнє положення та налаштує під них підрахунок повторів.\n\n"
        L"Секунди за повтор, розклад та все інше - у «Налаштуваннях».",
        L"Результат",
        L"Ці пороги буде записано в config.ini. Пропущені вправи зберігають поточні налаштування.",
        L"коліно %d\x00B0",
        L"лікоть %d\x00B0",
        L"пропущено",
        L"-",
        L"Камеру не знайдено. Підключіть камеру й запустіть калібрування знову.",
        L"Камера зайнята або недоступна. Закрийте інші програми, що її використовують, і виберіть камеру ще раз.",
        L"Камера не передає зображення. Виберіть іншу камеру у списку вище або закрийте інші програми, що її використовують.",
        L"Не вдалося завантажити модель пози - чи завершено встановлення?",
        L"Збережено в config.ini.",
        L" Службу перезапущено.",
        L" Перезапустіть службу (або комп'ютер), щоб застосувати.",
        L"Не вдалося записати config.ini (помилка %lu). Запустіть калібрування від імені адміністратора.",
        L"Нічого зберігати - усі вправи пропущено.",
        L"Присідання (кут коліна)\n  стійка %s \x2192 %d, спуск %s \x2192 %d\n  підйом %s \x2192 %d, низ %s \x2192 %d",
        L"Віджимання (кут ліктя)\n  угорі %s \x2192 %d, спуск %s \x2192 %d\n  підйом %s \x2192 %d, низ %s \x2192 %d",
        L"Мінімальне опускання стегон %s \x2192 %.2f",
        L"%s: без змін",
        L"Камера: %s",
        L"Приклад - куди поставити камеру:",
        L"Приклад - так має бачити камера:",
        L"2-3 м",
        L"Почати знову",
        L"Повтор зараховується, коли коліно згинається до %d\x00B0 або менше й випрямляється до %d\x00B0 або більше.",
        L"Повтор зараховується, коли лікоть згинається до %d\x00B0 або менше й випрямляється до %d\x00B0 або більше.",
        L"Присід не розпізнано: користувача має бути видно повністю, від голови до ніг.",
        L"Віджимання не розпізнано: має бути видно все тіло, руки - на підлозі.",
        L"стегно %d\x00B0",
        L"Прес (кут у стегні)\n  лежачи %s \x2192 %d, підйом %s \x2192 %d\n  опускання %s \x2192 %d, угорі %s \x2192 %d",
        L"Повтор зараховується, коли кут у стегні зменшується до %d\x00B0 або менше й повертається до %d\x00B0 або більше.",
        L"Положення для преса не розпізнано: користувач лежить на спині боком до камери, коліна зігнуті, усе тіло в кадрі.",
        L"Налаштування",
        L"Налаштування збережено.",
        L"Телефон (QR-код)",
        L"Відскануйте цей код камерою телефона й натисніть на телефоні «Почати». Потім поставте телефон туди, де він стоятиме "
        L"під час вправ, - на підлогу за 2-3 м, користувача має бути видно збоку повністю. Адресу нижче можна також ввести вручну.",
        L"Підключення до сервера...",
        L"Немає зв'язку з сервером %s - телефону потрібен інтернет на цьому комп'ютері. Виберіть вебкамеру або перевірте з'єднання, "
        L"а потім виберіть телефон ще раз.",
        L"Телефон підключено - чекаємо на його камеру. Натисніть на телефоні «Почати».",
        L"Телефон і вебкамера калібруються окремо: зроблені досі повтори скинуто.",
        L"Зберігається як власні налаштування телефона ([phone_squat], [phone_pushup], [phone_situp] у config.ini); налаштування вебкамери не змінюються.",
        L"Телефон підключено. Поставте його туди, де він стоятиме під час вправ: на підлогу за 2-3 м, боком до користувача, "
        L"щоб у кадрі було все тіло від голови до ніг.\n\n"
        L"Потім натисніть «Далі»: по 5 повторів кожної вправи у звичному темпі, а Exercise Unlock сам візьме з них положення. "
        L"Усе можна робити з цього телефона.",
        L"Присідання",
        L"Користувач стає боком до камери, усе тіло в кадрі, і робить 5 присідань у звичному темпі - так, як присідатиме щодня. Exercise Unlock сам візьме з них найвище й найнижче положення.\n\n"
        L"Присідайте по-справжньому, стегна приблизно паралельно підлозі: саме такі присідання й зараховуватимуться.",
        L"Віджимання",
        L"Камера стоїть на підлозі. Користувач боком до неї робить 5 віджимань у звичному темпі: руки випрямляються повністю, груди опускаються до підлоги. Exercise Unlock сам візьме з них верхнє й нижнє положення.",
        L"Прес",
        L"Камера стоїть на підлозі. Користувач лягає на спину боком до неї, коліна зігнуті, і робить 5 підйомів корпусу у звичному темпі. Exercise Unlock сам візьме з них положення лежачи й угорі.",
        L"Зроблено повторів: %d із %d",
        L"Готово: угорі %s, унизу %s.",
        L"Переходимо до наступного кроку...",
        L"Присідання надто неглибокі: коліно згиналося лише до %d\x00B0, а має хоча б до %d\x00B0. "
        L"Присідайте глибше й зробіть 5 присідань ще раз.",
        L"Замало руху: між верхнім і нижнім положенням лише %d\x00B0. Зробіть 5 повторів ще раз, з повною амплітудою, "
        L"боком до камери.",
        L"Стегна майже не опускаються в кадрі: мабуть, камера бачить користувача під невдалим кутом. Поставте її боком до "
        L"користувача й зробіть 5 присідань ще раз.",
        L"%s: угорі %s, унизу %s",
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

    // The hips' height in the frame (0 top .. 1 bottom), the way SquatDetector
    // reads it: the squat's hip-drop gate is calibrated from it.
    float HipY(const BodyPose& p)
    {
        float sum = 0.0f;
        int n = 0;
        for (int i : { kLHip, kRHip })
            if (p.joints[i].confidence >= kSquatConf) { sum += p.joints[i].y; ++n; }
        return n ? sum / static_cast<float>(n) : -1.0f;
    }

    // ------------------------------------------------------------ state
    struct Frame
    {
        std::vector<uint8_t> bgra;   // top-down BGRA
        uint32_t w = 0, h = 0;
        BodyPose pose{};
    };

    struct CameraInfo { std::wstring name, link; };

    // Each exercise: one step of 5 reps; their turning points give its
    // thresholds (CheckJudge.h). A new exercise adds its step here, an
    // ExerciseInfo, its Rules, its Plan / Save lines and detector.
    enum class StepKind { Camera, Reps, Summary };
    enum Exercise { kSquat = 0, kPushUp = 1, kSitUp = 2, kExerciseCount };
    struct StepDef
    {
        StepKind kind;
        int      exercise;   // Reps: an Exercise, else -1
        int      example;    // example picture: 0 camera scene, 1..6 a figure, -1 none
        Txt      title, text;
    };
    constexpr StepDef kStepDefs[] = {
        { StepKind::Camera,  -1,       0, tTitle0,         tText0 },
        { StepKind::Reps,    kSquat,   2, tTitleRepsSquat, tTextRepsSquat },
        { StepKind::Reps,    kPushUp,  4, tTitleRepsPush,  tTextRepsPush },
        { StepKind::Reps,    kSitUp,   6, tTitleRepsSit,   tTextRepsSit },
        { StepKind::Summary, -1,      -1, tTitle5,         tText5 },
    };

    // Per-exercise texts and detector bits.
    struct ExerciseInfo
    {
        const wchar_t* logName;
        Txt   angle;             // "knee %d°" ...
        Txt   summary;           // summary block
        Txt   threshold;         // "a rep counts when ..."
        Txt   noPose;            // the exercise's pose not recognized
        Ipc::PostureKind posture;
        const wchar_t* nameEn;
        const wchar_t* nameUk;
    };
    const ExerciseInfo kExercises[kExerciseCount] = {
        { L"squat",  tKnee,  tSumSquat, tTestThrSquat, tTestNoSquat, Ipc::PostureKind::Squat,  L"Squats",   L"Присідання" },
        { L"pushup", tElbow, tSumPush,  tTestThrPush,  tTestNoPlank, Ipc::PostureKind::PushUp, L"Push-ups", L"Віджимання" },
        { L"situp",  tHip,   tSumSitup, tTestThrSitup, tTestNoSitup, Ipc::PostureKind::SitUp,  L"Sit-ups",  L"Прес" },
    };
    constexpr int kSteps = static_cast<int>(std::size(kStepDefs));
    constexpr int kCalibReps = 5;            // reps per exercise
    constexpr UINT WM_APP_FRAME  = WM_APP + 1;
    constexpr UINT WM_APP_CAMERR = WM_APP + 2;
    constexpr UINT_PTR kTimerId  = 1;

    enum CtlId { idCamera = 100, idBack, idTake, idSkip, idNext, idUrl };

    HINSTANCE g_inst = nullptr;
    HWND g_wnd = nullptr, g_cam = nullptr, g_back = nullptr, g_take = nullptr, g_skip = nullptr, g_next = nullptr;
    HWND g_url = nullptr;              // the phone page's address under its QR code, to type in
    HFONT g_fontUi = nullptr, g_fontTitle = nullptr, g_fontBig = nullptr;
    UINT g_dpi = 96;
    bool g_setupMode = false;
    bool g_startPhone = false;        // /phone
    std::wstring g_snapshotPath;      // developer option: render one step to a .bmp and exit
    std::wstring g_usersPath;         // developer option (/users): another users.allow for the settings
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
    std::wstring g_status;            // error / info line under the instructions
    bool g_statusIsError = false;
    // An exercise whose reps gave its thresholds moves on to the next step by
    // itself, kAutoNextMs later (its result shows meanwhile), so nobody has to
    // walk to the computer.
    ULONGLONG g_autoNextAt = 0;       // 0: no move pending
    constexpr ULONGLONG kAutoNextMs = 2500;
    bool g_saved = false;

    const StepDef& Cur() { return kStepDefs[g_step]; }

    // ------------------------------------------------------------ reps
    // While a reps step is open the camera thread runs the service's own
    // detectors on every frame (RepsFrame): their posture latch, and the
    // exercise's smoothed angle, whose turning points (Swing) are the
    // child's reps. The UI thread only reads what it published.
    struct RepsFeed
    {
        uint32_t gen = 0;
        float    angle = 0.0f;
        bool     posture = false;          // the exercise's pose is latched now
        std::vector<float> peaks, valleys, peakHips, valleyHips;
    };
    std::atomic<int>      g_repsExercise{ -1 };   // detectors run for this one; -1 = off
    std::atomic<uint32_t> g_repsGen{ 0 };         // bumped = count from zero
    std::mutex            g_repsMx;
    RepsFeed              g_repsFeed;             // under g_repsMx

    // One exercise's calibration: the reps seen in this try, then what they gave.
    struct Capture
    {
        bool done = false, skipped = false;
        int  reps = 0;                         // reps seen in this try
        int  tries = 0;                        // tries that gave no thresholds
        ExerciseUnlock::Calib::RepPlan plan;   // done: the thresholds
        std::wstring fault;                    // why the last try gave none
        float angle = 0.0f;                    // the detector's angle now
        ULONGLONG postureOkMs = 0;             // last frame the exercise's pose was recognized
    };
    Capture g_caps[kExerciseCount];
    double g_aspect = 0.0;   // w / h of the webcam's frames the reps were done in

    // The camera thread's own detectors; same frame order as the
    // service's StreamPose.
    struct RepsDetectors
    {
        SquatDetector     squats;
        PushUpDetector    pushups;
        SitUpDetector     situps;
        PostureClassifier posture;
        Swing             swing;
        uint32_t          gen = 0;
        bool              started = false;
    };

    void RepsFrame(RepsDetectors& d, const BodyPose& pose)
    {
        const int ex = g_repsExercise.load();
        if (ex < 0) return;
        const uint32_t gen = g_repsGen.load();
        if (!d.started || d.gen != gen)
        {
            d.squats.Reset();
            d.pushups.Reset();
            d.situps.Reset();
            d.posture.Reset();
            d.swing.Reset();
            d.swing.hyst = ex == kPushUp ? 8.0f : 15.0f;   // MoveNet reads push-up elbows with a small span
            d.gen = gen;
            d.started = true;
        }
        d.squats.Update(pose);
        d.pushups.Update(pose);
        d.situps.Update(pose);
        d.posture.Update(pose);

        auto& st = ExerciseUnlock::Svc::GetState();
        const float angle[kExerciseCount] = {
            st.squatKneeAngle.load(), st.pushUpElbowAngle.load(), st.sitUpHipAngle.load() };
        // The detector's angle only moves while its posture is latched.
        const bool latched = st.postureKind.load() == static_cast<uint8_t>(kExercises[ex].posture);
        if (latched) d.swing.Push(angle[ex], ex == kSquat ? HipY(pose) : 0.0f);

        std::lock_guard<std::mutex> lk(g_repsMx);
        g_repsFeed.gen = gen;
        g_repsFeed.angle = angle[ex];
        g_repsFeed.posture = latched;
        g_repsFeed.peaks = d.swing.peaks;
        g_repsFeed.valleys = d.swing.valleys;
        g_repsFeed.peakHips = d.swing.peakAux;
        g_repsFeed.valleyHips = d.swing.valleyAux;
    }

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
        RepsDetectors reps;
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
                        const bool predicted = g_poseOk && SUCCEEDED(g_pose.Predict(buf.data(), w, h, pose));
                        if (!predicted)
                            pose = BodyPose{};
                        const ULONGLONG poseMs = GetTickCount64() - poseStart;
                        if (shown == 0 || poseMs > 2000)
                            LogF(L"calibrate: pose took %llu ms", poseMs);
                        if (predicted && !s->stop.load())
                            RepsFrame(reps, pose);
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

    // ------------------------------------------------------------ phone
    // "Phone (QR code)" in the camera list: the calibration through the
    // phone camera, for the phone's own thresholds (Config::PhoneProfile).
    // A relay connection of its own - a new id and key each time, never the
    // service's - and a QR code with its token. The phone page (mode
    // "calib") streams its poses, which take the same path as a camera's
    // frames: no picture, the skeleton only.
    constexpr UINT WM_APP_PHONE    = WM_APP + 4;   // the connection changed
    constexpr UINT WM_APP_PHONENAV = WM_APP + 6;   // a wizard button pressed on the phone: wParam = its CtlId

    enum class PhoneState { Connecting, NoServer, Waiting, Connected };

    struct PhoneSession
    {
        std::atomic<bool> stop{ false };
        Relay::Socket     sock;
        std::wstring      relay;                     // config.ini [phone] relay, for the error text
        std::mutex        mx;                        // the three below
        PhoneState        state = PhoneState::Connecting;
        std::string       url;                       // what the QR code says, once the relay is ready
        std::string       sid;                       // the phone on the other end, "" = none
        HANDLE            done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~PhoneSession() { if (done) CloseHandle(done); }
    };
    std::shared_ptr<PhoneSession> g_phone;
    std::thread g_phoneThread;
    int         g_phoneItem = -1;       // its row in the camera list; -1 = no relay in config.ini
    bool        g_phoneMode = false;    // the reps and thresholds are the phone's
    ULONGLONG   g_phonePingMs = 0;
    std::string g_calibSent;            // the step last shown on the phone
    bool        g_sourceReady = false;

    PhoneState PhoneStateNow()
    {
        if (!g_phone) return PhoneState::NoServer;
        std::lock_guard<std::mutex> lk(g_phone->mx);
        return g_phone->state;
    }

    // To the phone on the other end, if there is one.
    void PhoneSend(PhoneSession& s, const std::string& msg)
    {
        std::string sid;
        {
            std::lock_guard<std::mutex> lk(s.mx);
            sid = s.sid;
        }
        if (sid.empty()) return;
        Json::Writer w;
        w.BeginObject().Key("t").String("to").Key("sid").String(sid).Key("msg").Raw(msg).EndObject();
        s.sock.Send(w.Str());
    }

    void PhoneThread(std::shared_ptr<PhoneSession> s, Relay::RelayUrl url)
    {
        auto setState = [&](PhoneState st) {
            {
                std::lock_guard<std::mutex> lk(s->mx);
                s->state = st;
            }
            if (!s->stop.load()) PostMessageW(g_wnd, WM_APP_PHONE, 0, 0);
        };
        DWORD err = 0;
        if (!s->sock.Open(url, err))
        {
            LogF(L"calibrate: relay %s unreachable (%lu)", s->relay.c_str(), err);
            setState(PhoneState::NoServer);
            SetEvent(s->done);
            return;
        }
        {
            Json::Writer w;
            w.BeginObject().Key("t").String("hello").Key("pc").String(Relay::RandomBase32(16))
             .Key("key").String(Relay::RandomBase32(20)).Key("v").Int(1)
             .Key("app").String(Relay::ToUtf8(ExerciseUnlock::kVersionString)).EndObject();
            s->sock.Send(w.Str());
        }

        RepsDetectors reps;
        std::string text;
        while (!s->stop.load() && s->sock.Receive(text))
        {
            Json::Value m;
            if (!Json::Parser::Parse(text, m) || !m.IsObject()) continue;
            const std::string t = m.Str("t");
            if (t == "ready")
            {
                const std::string token = Relay::RandomBase32(10);
                Json::Writer w;
                w.BeginObject().Key("t").String("offer").Key("token").String(token).EndObject();
                s->sock.Send(w.Str());
                {
                    std::lock_guard<std::mutex> lk(s->mx);
                    s->url = url.qrBase + token;
                }
                LogF(L"calibrate: phone code %hs", (url.qrBase + token).c_str());
                setState(PhoneState::Waiting);
            }
            else if (t == "join")
            {
                const std::string sid = m.Str("sid");
                bool taken = false;
                {
                    std::lock_guard<std::mutex> lk(s->mx);
                    taken = !s->sid.empty();
                    if (!taken) s->sid = sid;
                }
                if (taken)
                {
                    // One phone measures; another scan of the code is turned away.
                    Json::Writer w;
                    w.BeginObject().Key("t").String("kick").Key("sid").String(sid).Key("reason").String("busy").EndObject();
                    s->sock.Send(w.Str());
                    continue;
                }
                // lang: the phone page speaks the wizard's language, whatever the phone is set to
                PhoneSend(*s, g_uk ? R"({"t":"config","v":1,"mode":"calib","poses":1,"space":"phone","exercises":[],"lang":"uk"})"
                                   : R"({"t":"config","v":1,"mode":"calib","poses":1,"space":"phone","exercises":[],"lang":"en"})");
                LogF(L"calibrate: phone joined");
                setState(PhoneState::Connected);
            }
            else if (t == "leave")
            {
                bool ours = false;
                {
                    std::lock_guard<std::mutex> lk(s->mx);
                    ours = s->sid == m.Str("sid");
                    if (ours) s->sid.clear();
                }
                if (!ours) continue;
                {
                    std::lock_guard<std::mutex> lk(g_frameMx);
                    g_latest = Frame{};
                }
                LogF(L"calibrate: phone left");
                setState(PhoneState::Waiting);
            }
            else if (t == "from")
            {
                const Json::Value* msg = m.Get("msg");
                if (!msg || !msg->IsObject()) continue;
                {
                    std::lock_guard<std::mutex> lk(s->mx);
                    if (m.Str("sid") != s->sid) continue;
                }
                const std::string mt = msg->Str("t");
                if (mt == "pose")
                {
                    BodyPose pose{};
                    uint32_t w = 0, h = 0;
                    uint64_t ts = 0;
                    if (!ExerciseUnlock::Svc::PhoneCounter::ParsePose(*msg, pose, w, h, ts)) continue;
                    RepsFrame(reps, pose);
                    {
                        std::lock_guard<std::mutex> lk(g_frameMx);
                        g_latest.bgra.clear();   // the phone sends no picture
                        g_latest.w = w;
                        g_latest.h = h;
                        g_latest.pose = pose;
                    }
                    g_lastFrameMs = GetTickCount64();
                    if (!g_framePending.exchange(true)) PostMessageW(g_wnd, WM_APP_FRAME, 0, 0);
                }
                else if (mt == "nav")
                {
                    const std::string id = msg->Str("id");
                    const int cmd = id == "back" ? idBack : id == "next" ? idNext : id == "skip" ? idSkip : id == "take" ? idTake : 0;
                    if (cmd) PostMessageW(g_wnd, WM_APP_PHONENAV, static_cast<WPARAM>(cmd), 0);
                }
                else if (mt == "hello")
                    LogF(L"calibrate: phone %hs", msg->Str("ua").substr(0, 100).c_str());
                else if (mt == "diag")
                {
                    // The phone's camera, model and speed (every 10 s), and
                    // how sure the model is of the legs: why reps go unseen.
                    const std::string kind = msg->Str("kind");
                    if (kind == "start" || kind == "tick")
                    {
                        const Json::Value* c = msg->Get("conf");
                        const auto conf = [&](const char* j) {
                            return c && c->IsObject() ? static_cast<int>(std::lround(c->Num(j, -0.01) * 100.0)) : -1;
                        };
                        LogF(L"calibrate: phone %hs %hs %dx%d %hs fps=%.1f hips %d/%d knees %d/%d ankles %d/%d",
                             kind.c_str(), msg->Str("backend").substr(0, 20).c_str(),
                             static_cast<int>(msg->Num("w", 0.0)), static_cast<int>(msg->Num("h", 0.0)),
                             msg->Str("facing").substr(0, 20).c_str(), msg->Num("fps", 0.0),
                             conf("lhip"), conf("rhip"), conf("lknee"), conf("rknee"), conf("lankle"), conf("rankle"));
                    }
                    else if (kind == "startfail" || kind == "error")
                        LogF(L"calibrate: phone %hs %hs", kind.c_str(), msg->Str("msg").substr(0, 100).c_str());
                }
            }
        }
        if (!s->stop.load())
        {
            LogF(L"calibrate: relay connection lost");
            {
                std::lock_guard<std::mutex> lk(s->mx);
                s->sid.clear();
                s->url.clear();
            }
            {
                std::lock_guard<std::mutex> lk(g_frameMx);
                g_latest = Frame{};
            }
            setState(PhoneState::NoServer);
        }
        SetEvent(s->done);
    }

    void UpdatePhoneUrl()
    {
        std::string url;
        bool show = false;
        if (g_phone)
        {
            std::lock_guard<std::mutex> lk(g_phone->mx);
            show = g_phone->state == PhoneState::Waiting && !g_phone->url.empty();
            url = g_phone->url;
        }
        SetWindowTextW(g_url, Relay::ToWide(url).c_str());
        ShowWindow(g_url, show ? SW_SHOWNA : SW_HIDE);
    }

    void StopPhone()
    {
        if (std::shared_ptr<PhoneSession> s = std::move(g_phone))
        {
            s->stop = true;
            PhoneSend(*s, g_saved ? R"({"t":"bye","reason":"saved"})" : R"({"t":"bye","reason":"closed"})");
            s->sock.Shutdown();                                    // the relay answers, Receive returns
            if (WaitForSingleObject(s->done, 2000) != WAIT_OBJECT_0) s->sock.Abort();
            if (g_phoneThread.joinable())
            {
                if (WaitForSingleObject(s->done, 2000) == WAIT_OBJECT_0) g_phoneThread.join();
                else g_phoneThread.detach();
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_frameMx);
            g_latest = Frame{};
        }
        if (g_url) UpdatePhoneUrl();
    }

    void StartPhone()
    {
        StopPhone();
        auto s = std::make_shared<PhoneSession>();
        s->relay = Config::PhoneRelayUrl();
        Relay::RelayUrl url;
        g_phone = s;
        g_calibSent.clear();
        g_phonePingMs = GetTickCount64();
        if (!Relay::ParseRelay(s->relay, url))
        {
            s->state = PhoneState::NoServer;
            SetEvent(s->done);
            return;
        }
        LogF(L"calibrate: phone source via %s", s->relay.c_str());
        g_phoneThread = std::thread(PhoneThread, s, url);
    }

    // The phone source before its first pose: the QR code to scan, or what
    // keeps it from showing.
    void PaintPhoneWait(HDC dc, const RECT& area)
    {
        PhoneState st = PhoneState::NoServer;
        std::string url;
        if (g_phone)
        {
            std::lock_guard<std::mutex> lk(g_phone->mx);
            st = g_phone->state;
            url = g_phone->url;
        }
        const std::wstring text = st == PhoneState::NoServer   ? Fmt(T(tPhoneNoServer), g_phone ? g_phone->relay.c_str() : L"")
                                : st == PhoneState::Connecting ? std::wstring(T(tPhoneServer))
                                : st == PhoneState::Connected  ? std::wstring(T(tPhoneWaiting))
                                                               : std::wstring(T(tPhoneScan));
        int textTop = area.top + Px(40);
        Qr::Code code;
        if (st == PhoneState::Waiting && Qr::Encode(url, code))
        {
            constexpr int kQuiet = 4;
            const int avail = std::min(area.right - area.left, area.bottom - area.top - Px(190));
            const int m = std::max(2, avail / (code.size + 2 * kQuiet));
            const int px = (code.size + 2 * kQuiet) * m;
            const int ox = area.left + (area.right - area.left - px) / 2, oy = area.top + Px(24);
            const RECT bg{ ox, oy, ox + px, oy + px };
            HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(dc, &bg, white);
            for (int y = 0; y < code.size; ++y)
                for (int x = 0; x < code.size; ++x)
                {
                    if (!code.At(x, y)) continue;
                    const RECT r{ ox + (kQuiet + x) * m, oy + (kQuiet + y) * m, ox + (kQuiet + x + 1) * m, oy + (kQuiet + y + 1) * m };
                    FillRect(dc, &r, black);
                }
            DeleteObject(white);
            DeleteObject(black);
            textTop = oy + px + Px(14);
        }
        RECT tr{ area.left + Px(32), textTop, area.right - Px(32), area.bottom - Px(56) };
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(220, 220, 230));
        DrawTextW(dc, text.c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK);
    }

    // ------------------------------------------------------------ thresholds
    // One exercise's four thresholds, as the detectors name them: top (rest
    // pose: standing / arms straight / lying), going down, going up, down
    // (squatting / bottom of the push-up / sitting up).
    struct ExercisePlan
    {
        bool ok = false;
        int top = 0, goingDown = 0, goingUp = 0, down = 0;
    };
    struct Plan
    {
        ExercisePlan ex[kExerciseCount];
        bool  hip = false;       // squat: a hip-drop gate from the reps
        float hipDrop = 0.0f;
    };

    // Where each exercise's thresholds live in config.ini, in the order above.
    struct IniKeys { const wchar_t* section; const wchar_t* key[4]; };
    const IniKeys kIni[kExerciseCount] = {
        { L"squat",  { L"standing_angle", L"going_down_angle", L"going_up_angle", L"down_angle" } },
        { L"pushup", { L"up_angle",       L"going_down_angle", L"going_up_angle", L"down_angle" } },
        { L"situp",  { L"lying_angle",    L"rising_angle",     L"lowering_angle", L"sitting_angle" } },
    };

    // Where an exercise's thresholds go: the webcam's sections, or the
    // phone's own ([phone_squat] ..., read by Config::PhoneProfile).
    const wchar_t* Section(int ex)
    {
        static const wchar_t* const kPhone[kExerciseCount] = { L"phone_squat", L"phone_pushup", L"phone_situp" };
        return g_phoneMode ? kPhone[ex] : kIni[ex].section;
    }

    const ExerciseUnlock::Calib::Rules& RulesOf(int ex)
    {
        using namespace ExerciseUnlock::Calib;
        return ex == kSquat ? kSquatRules : ex == kPushUp ? kPushupRules : kSitupRules;
    }

    const wchar_t* ExerciseName(int ex) { return kExercises[ex].logName; }

    // The thresholds the finished exercises gave.
    Plan ComputePlan()
    {
        Plan p;
        for (int ex = 0; ex < kExerciseCount; ++ex)
        {
            const Capture& c = g_caps[ex];
            if (!c.done) continue;
            ExercisePlan& e = p.ex[ex];
            e.ok = true;
            e.top = c.plan.t.top;
            e.goingDown = c.plan.t.goingDown;
            e.goingUp = c.plan.t.goingUp;
            e.down = c.plan.t.down;
        }
        if (g_caps[kSquat].done && g_caps[kSquat].plan.t.hipDrop > 0.0f)
        {
            p.hip = true;
            p.hipDrop = g_caps[kSquat].plan.t.hipDrop;
        }
        return p;
    }

    void StopReps() { g_repsExercise = -1; }

    // A new try at the exercise's reps, counted from zero.
    void StartReps(int ex)
    {
        Capture& c = g_caps[ex];
        c.done = c.skipped = false;
        c.reps = 0;
        c.plan = {};
        c.postureOkMs = GetTickCount64();
        ++g_repsGen;
        g_repsExercise = ex;
        LogF(L"calibrate: %s reps started", kExercises[ex].logName);
    }

    std::wstring AngleText(int ex, int v) { return Fmt(T(kExercises[ex].angle), v); }

    // What the reps gave, in the parent's words.
    std::wstring RepsResult(int ex)
    {
        const Capture& c = g_caps[ex];
        return Fmt(T(tRepsDone), AngleText(ex, c.plan.top).c_str(), AngleText(ex, c.plan.bottom).c_str());
    }

    void UpdateControls();

    // UI thread, per frame: take over what the detectors published. The
    // fifth rep ends the try: thresholds, or why there are none and 5 more.
    void PollReps()
    {
        const int ex = Cur().exercise;
        if (Cur().kind != StepKind::Reps || g_repsExercise.load() != ex) return;
        RepsFeed f;
        {
            std::lock_guard<std::mutex> lk(g_repsMx);
            f = g_repsFeed;
        }
        if (f.gen != g_repsGen.load()) return;   // the previous try's numbers
        Capture& c = g_caps[ex];
        c.angle = f.angle;
        if (f.posture) c.postureOkMs = GetTickCount64();
        const int reps = std::min(static_cast<int>(f.valleys.size()), kCalibReps);
        if (reps != c.reps)
        {
            c.reps = reps;
            if (reps > 0) c.fault.clear();   // a new try is under way
        }
        if (reps < kCalibReps) return;

        using ExerciseUnlock::Calib::RepFault;
        const auto r = ExerciseUnlock::Calib::PlanFromReps(f.peaks, f.valleys, f.peakHips, f.valleyHips, RulesOf(ex));
        LogF(L"calibrate: %s reps reach %d..%d -> %d/%d/%d/%d hip %.2f fault %d", kExercises[ex].logName,
             r.bottom, r.top, r.t.top, r.t.goingDown, r.t.goingUp, r.t.down, r.t.hipDrop, static_cast<int>(r.fault));
        if (r.fault != RepFault::None)
        {
            ++c.tries;
            c.fault = r.fault == RepFault::Shallow ? Fmt(T(tRepsShallow), r.bottom, RulesOf(ex).deepest)
                    : r.fault == RepFault::Hips    ? std::wstring(T(tRepsHips))
                                                   : Fmt(T(tRepsRange), r.range);
            StartReps(ex);
            UpdateControls();
            return;
        }
        c.done = true;
        c.plan = r;
        c.fault.clear();
        StopReps();
        if (!g_phoneMode)
        {
            std::lock_guard<std::mutex> lk(g_frameMx);
            if (g_latest.w > 0 && g_latest.h > 0) g_aspect = static_cast<double>(g_latest.w) / g_latest.h;
        }
        g_autoNextAt = GetTickCount64() + kAutoNextMs;
        UpdateControls();
    }

    struct Para { std::wstring text; COLORREF color; };
    constexpr COLORREF kInfoInk = RGB(90, 96, 130), kOkInk = RGB(20, 150, 110), kErrInk = RGB(200, 40, 60),
                       kWarnInk = RGB(200, 110, 20), kTextInk = RGB(31, 37, 71);

    // The reps step: how many so far and what went wrong, or what came out.
    std::vector<Para> RepsParas(int ex)
    {
        std::vector<Para> out;
        const Capture& c = g_caps[ex];
        if (c.done)
        {
            out.push_back({ RepsResult(ex), kOkInk });
            out.push_back({ Fmt(T(kExercises[ex].threshold), c.plan.t.down, c.plan.t.top), kInfoInk });
            if (g_autoNextAt) out.push_back({ T(tRepsNext), kInfoInk });
            return out;
        }
        if (!c.fault.empty()) out.push_back({ c.fault, kErrInk });
        out.push_back({ Fmt(T(tRepsCount), c.reps, kCalibReps), kTextInk });
        if (g_repsExercise.load() == ex && GetTickCount64() - c.postureOkMs > 2500)
            out.push_back({ T(kExercises[ex].noPose), kWarnInk });
        return out;
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
        for (int ex = 0; ex < kExerciseCount; ++ex)
        {
            const ExercisePlan& e = p.ex[ex];
            const IniKeys& k = kIni[ex];
            if (ex) s += L"\n\n";
            if (!e.ok)
            {
                s += Fmt(T(tSumKept), g_uk ? kExercises[ex].nameUk : kExercises[ex].nameEn);
                continue;
            }
            const wchar_t* sec = Section(ex);
            s += Fmt(T(kExercises[ex].summary),
                     OldVal(sec, k.key[0]).c_str(), e.top, OldVal(sec, k.key[1]).c_str(), e.goingDown,
                     OldVal(sec, k.key[2]).c_str(), e.goingUp, OldVal(sec, k.key[3]).c_str(), e.down);
            if (ex == kSquat && p.hip)
                s += L"\n" + Fmt(T(tSumHip), OldVal(Section(kSquat), L"min_hip_drop").c_str(), p.hipDrop);
        }
        const int cam = static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0));
        if (g_phoneMode)
            s += L"\n\n" + Fmt(T(tSumCamera), T(tPhone)) + L"\n" + T(tSumPhone);
        else if (cam >= 0 && cam < static_cast<int>(g_cameras.size()))
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

    ExerciseUnlock::Calib::SettingsRequest SettingsFor(HWND owner)
    {
        ExerciseUnlock::Calib::SettingsRequest req;
        req.inst = g_inst;
        req.owner = owner;
        req.uk = g_uk;
        req.configPath = g_configPath;
        req.soundsPath = ExerciseUnlock::DataPath(L"sounds.ini");
        req.usersPath = g_usersPath.empty() ? ExerciseUnlock::DataPath(L"users.allow") : g_usersPath;
        req.adminToolPath = g_exeDir + L"\\ExerciseUnlockAdmin.exe";
        for (const CameraInfo& c : g_cameras) req.cameras.push_back(c.name);
        return req;
    }

    // The camera config.ini asks for, -1 when it names none of them.
    int PreferredCamera()
    {
        const std::wstring pref = IniGet(L"camera", L"preferred");
        for (size_t i = 0; i < g_cameras.size(); ++i)
            if (!pref.empty() && StrStrIW(g_cameras[i].name.c_str(), pref.c_str())) return static_cast<int>(i);
        return -1;
    }

    // From the first and the last step: it reads config.ini again.
    void OpenSettings()
    {
        const auto r = ExerciseUnlock::Calib::ShowSettings(SettingsFor(g_wnd));
        if (!r.saved) return;
        Config::Load();
        const int pick = PreferredCamera();
        if (!g_phoneMode && pick >= 0 && pick != static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0)))
        {
            SendMessageW(g_cam, CB_SETCURSEL, static_cast<WPARAM>(pick), 0);
            StartCamera(pick);
        }
        g_status = T(tSettingsSaved);
        if (r.serviceConfig && !g_setupMode) g_status += RestartService() ? T(tRestarted) : T(tRestartFailed);
        g_statusIsError = false;
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    bool Save()
    {
        const Plan p = ComputePlan();
        const int cam = static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0));
        const bool setCamera = !g_phoneMode && g_cameras.size() > 1 && cam >= 0 && cam < static_cast<int>(g_cameras.size());
        bool any = setCamera;
        for (const ExercisePlan& e : p.ex) any = any || e.ok;
        if (!any)
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
        for (int ex = 0; ex < kExerciseCount; ++ex)
        {
            const ExercisePlan& e = p.ex[ex];
            if (!e.ok) continue;
            const IniKeys& k = kIni[ex];
            const int v[4] = { e.top, e.goingDown, e.goingUp, e.down };
            for (int i = 0; i < 4; ++i) put(Section(ex), k.key[i], std::to_wstring(v[i]));
            if (ex == kSquat && p.hip) put(Section(kSquat), L"min_hip_drop", Fmt(L"%.2f", p.hipDrop));
            LogF(L"calibrate: saved %s %s %d/%d/%d/%d", g_phoneMode ? L"phone" : L"webcam", ExerciseName(ex),
                 e.top, e.goingDown, e.goingUp, e.down);
        }
        // Only pin the camera when there is a choice to make: with a
        // single camera a pinned name would just make the service wait
        // if it ever gets renamed / replaced.
        if (setCamera) put(L"camera", L"preferred", g_cameras[static_cast<size_t>(cam)].name);
        // The angles are measured on x / width, y / height of this camera's
        // frame, so they depend on its aspect ratio. The phone camera page
        // rescales its poses to it (service PhoneLink sends it along) - unless
        // the phone has thresholds of its own, measured on its own frame.
        if (!g_phoneMode && g_aspect > 0.0) put(L"calibration", L"aspect", Fmt(L"%.4f", g_aspect));
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_configPath.c_str());   // flush

        if (!ok)
        {
            g_status = Fmt(T(tSaveFailed), GetLastError());
            g_statusIsError = true;
            return false;
        }
        if (p.hip) LogF(L"calibrate: saved squat hip drop %.2f", p.hipDrop);
        if (g_phone) PhoneSend(*g_phone, R"({"t":"bye","reason":"saved"})");
        g_status = T(tSaved);
        if (!g_setupMode) g_status += RestartService() ? T(tRestarted) : T(tRestartFailed);
        g_statusIsError = false;
        return true;
    }

    // ------------------------------------------------------------ frames
    void SendCalibStep();

    void OnFrame()
    {
        g_framePending = false;
        if (g_status == T(tNoFrames)) g_status.clear();   // the picture came after all
        PollReps();
        if (Cur().kind == StepKind::Reps) SendCalibStep();   // sent only when it changed
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    // ------------------------------------------------------------ UI
    // Something to measure on: a camera, or a phone that sends its poses.
    bool SourceReady()
    {
        if (!g_phoneMode) return !g_cameras.empty();
        return PhoneStateNow() == PhoneState::Connected && GetTickCount64() - g_lastFrameMs.load() < 3000;
    }

    // The wizard's step on the phone's screen: what to do now, the wizard's
    // own buttons (a page from 1.5.2 shows them), and on a reps step how many
    // reps so far and what came out.
    const char* ParaKind(COLORREF c)
    {
        return c == kOkInk ? "ok" : c == kErrInk ? "err" : c == kWarnInk ? "warn" : c == kInfoInk ? "info" : "text";
    }

    // The summary on the phone: one line per exercise, and the save's result.
    std::vector<Para> SummaryParas()
    {
        std::vector<Para> out;
        for (int ex = 0; ex < kExerciseCount; ++ex)
        {
            const std::wstring name = g_uk ? kExercises[ex].nameUk : kExercises[ex].nameEn;
            if (g_caps[ex].done)
                out.push_back({ Fmt(T(tSumReps), name.c_str(), AngleText(ex, g_caps[ex].plan.top).c_str(),
                                    AngleText(ex, g_caps[ex].plan.bottom).c_str()), kOkInk });
            else out.push_back({ Fmt(T(tSumKept), name.c_str()), kWarnInk });
        }
        if (!g_status.empty()) out.push_back({ g_status, g_statusIsError ? kErrInk : kOkInk });
        return out;
    }

    void SendCalibStep()
    {
        if (!g_phone) return;
        const StepDef& s = Cur();
        const bool reps = s.kind == StepKind::Reps;
        // The first step speaks to the phone itself.
        const std::wstring text = s.kind == StepKind::Camera && g_phoneMode ? std::wstring(T(tPhoneStep0)) : std::wstring(T(s.text));
        Json::Writer w;
        w.BeginObject().Key("t").String("calib")
         .Key("title").String(Relay::ToUtf8(T(s.title)))
         .Key("text").String(Relay::ToUtf8(text))
         .Key("photo").Bool(false)            // these four keep a page before 1.5.2 from offering a photo
         .Key("busy").Bool(!SourceReady())
         .Key("count").Int(0)
         .Key("measuring").Bool(false)
         .Key("shot").Int(0)
         .Key("test").String(reps ? Relay::ToUtf8(kExercises[s.exercise].logName) : std::string());
        // The wizard's own buttons, as the computer shows them: the phone drives the whole calibration.
        // "Skip" on the first and the last step opens the settings: that one stays on the computer.
        const bool pending = g_autoNextAt != 0;
        auto button = [&](const char* key, HWND h, bool offer) {
            wchar_t label[64] = L"";
            GetWindowTextW(h, label, 64);
            w.Key(key).BeginObject()
             .Key("label").String(Relay::ToUtf8(label))
             .Key("show").Bool(offer && IsWindowVisible(h))
             .Key("on").Bool(offer && IsWindowEnabled(h) && !pending)
             .EndObject();
        };
        w.Key("nav").BeginObject();
        button("back", g_back, true);
        button("skip", g_skip, reps);
        button("take", g_take, reps);
        button("next", g_next, true);
        w.EndObject();
        auto lines = [&](const std::vector<Para>& paras) {
            w.Key("lines").BeginArray();
            for (const Para& para : paras)
                w.BeginObject().Key("text").String(Relay::ToUtf8(para.text)).Key("kind").String(ParaKind(para.color)).EndObject();
            w.EndArray();
        };
        if (reps)
        {
            // the reps so far; a try that gave no thresholds (att) sounds on the phone
            const Capture& c = g_caps[s.exercise];
            w.Key("reps").Int(c.done ? kCalibReps : c.reps).Key("repsOf").Int(kCalibReps).Key("passed").Bool(c.done)
             .Key("att").Int(c.tries).Key("attOk").Bool(false);
            lines(RepsParas(s.exercise));
        }
        else if (s.kind == StepKind::Summary)
            lines(SummaryParas());
        w.EndObject();
        if (w.Str() == g_calibSent) return;
        g_calibSent = w.Str();
        PhoneSend(*g_phone, g_calibSent);
    }

    void UpdateControls()
    {
        const StepDef& s = Cur();
        const bool cameraStep = s.kind == StepKind::Camera;
        const bool repsStep = s.kind == StepKind::Reps;
        const bool summary = s.kind == StepKind::Summary;
        const bool done = repsStep && g_caps[s.exercise].done;
        EnableWindow(g_back, g_step > 0 && !(summary && g_saved));
        ShowWindow(g_take, repsStep ? SW_SHOW : SW_HIDE);
        SetWindowTextW(g_take, T(tRestart));
        EnableWindow(g_take, repsStep && SourceReady());
        // With nothing to skip, the button opens the settings.
        SetWindowTextW(g_skip, T(cameraStep || summary ? tSettings : tSkip));
        EnableWindow(g_skip, !done);
        SetWindowTextW(g_next, T(summary ? (g_saved ? tClose : tSave) : tNext));
        EnableWindow(g_next, !repsStep || done);
        SendCalibStep();
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    void GoToStep(int step)
    {
        step = std::clamp(step, 0, kSteps - 1);
        StopReps();
        g_autoNextAt = 0;
        g_step = step;
        g_status.clear();
        // An exercise not done yet counts from here at once; a done one shows
        // what its reps gave ("Start over" for new ones).
        if (Cur().kind == StepKind::Reps && !g_caps[Cur().exercise].done)
        {
            g_caps[Cur().exercise] = Capture{};
            StartReps(Cur().exercise);
        }
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
        const RECT v = VideoRect(rc);
        MoveWindow(g_url, v.left + Px(24), v.bottom - Px(44), v.right - v.left - Px(48), Px(26), TRUE);
    }

    void MakeFonts()
    {
        for (HFONT f : { g_fontUi, g_fontTitle, g_fontBig }) if (f) DeleteObject(f);
        auto mk = [](int pt, int weight) {
            return CreateFontW(-MulDiv(pt, static_cast<int>(g_dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        };
        g_fontUi = mk(10, FW_NORMAL);
        g_fontTitle = mk(15, FW_SEMIBOLD);
        g_fontBig = mk(40, FW_BOLD);      // test rep counter over the video
        for (HWND h : { g_cam, g_back, g_take, g_skip, g_next })
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontUi), TRUE);
        if (g_url) SendMessageW(g_url, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontTitle), TRUE);
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

        const StepDef& s = Cur();
        Frame f;
        {
            std::lock_guard<std::mutex> lk(g_frameMx);
            f = g_latest;
        }

        SelectObject(dc, g_fontUi);
        if (f.bgra.empty() && (f.w == 0 || f.h == 0))
        {
            if (g_phoneMode)
            {
                PaintPhoneWait(dc, area);
                return;
            }
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
        if (f.bgra.empty())
        {
            // The phone sends no picture: its frame, with the skeleton on it.
            HBRUSH frame = CreateSolidBrush(RGB(14, 16, 26));
            FillRect(dc, &v, frame);
            DeleteObject(frame);
        }
        else
        {
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, v.left, v.top, vw, vh, 0, 0, static_cast<int>(f.w), static_cast<int>(f.h),
                          f.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        DrawSkeleton(dc, v, f.pose);

        // A reps step: the reps so far, and the angle the detector sees right now.
        if (s.kind == StepKind::Reps)
        {
            const Capture& c = g_caps[s.exercise];
            std::wstring big = Fmt(L"%d / %d", c.done ? kCalibReps : c.reps, kCalibReps);
            if (c.done) big += L"  \x2714";
            RECT br{ v.left + Px(16), v.top + Px(4), v.right, v.top + Px(70) };
            SelectObject(dc, g_fontBig);
            DrawShadowText(dc, big, br, DT_LEFT | DT_SINGLELINE, c.done ? RGB(90, 230, 140) : RGB(255, 230, 90));
            const bool seen = GetTickCount64() - c.postureOkMs < 1000;
            if (!c.done && seen && c.angle > 0)
            {
                RECT lr{ v.left + Px(18), v.top + Px(72), v.right, v.top + Px(104) };
                SelectObject(dc, g_fontTitle);
                DrawShadowText(dc, AngleText(s.exercise, static_cast<int>(std::lround(c.angle))), lr,
                               DT_LEFT | DT_SINGLELINE, RGB(255, 255, 255));
            }
        }
    }

    // ------------------------------------------------------------ example pictures
    // A side-view figure per example (StepDef::example): 0 shows where the
    // camera goes, 1-6 a pose the camera should see in the reps. Figure
    // units: y grows down,
    // the floor is at y = 100, the figure faces right.
    struct FigPt { float x, y; };
    struct Figure
    {
        FigPt head; float headR;
        FigPt neck, shoulder, elbow, wrist, hip, knee, ankle, toe;
        int ring;    // joint the step measures: 1 knee, 2 elbow, 3 hip
        FigPt eye;   // from the head's centre, on the side the face looks to
    };

    const Figure kFigures[6] = {
        // standing: legs straight, arms down
        { { 51, 15 }, 6.5f, { 50, 23 }, { 50, 27 }, { 52, 41 }, { 55, 54 }, { 50, 55 }, { 51, 76 }, { 50, 96 }, { 57, 97 }, 1, { 3, -1 } },
        // squat: thighs about level, arms forward
        { { 57, 40 }, 6.5f, { 54, 46 }, { 52, 50 }, { 65, 53 }, { 78, 52 }, { 40, 74 }, { 60, 77 }, { 50, 96 }, { 57, 97 }, 1, { 3, -1 } },
        // push-up, arms straight: hands under the shoulders, body in one line
        { { 88, 66 }, 6.5f, { 82, 70 }, { 78, 72 }, { 78, 85 }, { 78, 98 }, { 52, 80 }, { 33, 86 }, { 14, 92 }, { 11, 98 }, 2, { 3, 0.5f } },
        // push-up, bottom: chest low, elbows bent about 90 degrees and
        // sticking up above the back
        { { 96, 85 }, 6.5f, { 90, 88.5f }, { 86, 89.5f }, { 73, 84 }, { 71, 98 }, { 59, 91.5f }, { 39, 93 }, { 19, 94.5f }, { 16, 98.5f }, 2, { 3, 0.5f } },
        // sit-up, lying: on the back, knees bent with the feet flat, arms
        // crossed on the chest, face up
        { { 8, 92 }, 6.5f, { 15, 93.5f }, { 20, 94 }, { 30, 87 }, { 22, 86 }, { 48, 95 }, { 63, 80 }, { 77, 96 }, { 84, 98 }, 3, { 1.5f, -3 } },
        // sit-up, top: the same legs, the torso up and a little forward
        { { 58.5f, 56 }, 6.5f, { 56.5f, 64 }, { 55, 68 }, { 64, 75 }, { 57, 71 }, { 48, 95 }, { 63, 80 }, { 77, 96 }, { 84, 98 }, 3, { 3, -1 } },
    };

    // Scene extent in figure units (figure, plus camera and distance label on step 0).
    struct Bounds { float x0, x1, y0, y1; };
    Bounds FigureBounds(int example)
    {
        const bool scene = example == 0;
        const Figure& f = kFigures[scene ? 0 : example - 1];
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
        b.y1 = scene ? 118.0f : 104.0f;
        return b;
    }

    Bounds ExampleBounds(int example)
    {
        Bounds b = FigureBounds(example);
        // Both pictures of a floor exercise at the same scale, so they
        // compare at a glance.
        const int partner = example == 3 ? 4 : example == 4 ? 3 : example == 5 ? 6 : example == 6 ? 5 : -1;
        if (partner > 0)
        {
            const Bounds o = FigureBounds(partner);
            b.x0 = std::min(b.x0, o.x0);
            b.x1 = std::max(b.x1, o.x1);
            b.y0 = std::min(b.y0, o.y0);
        }
        return b;
    }
    constexpr float kExamplePad = 9.0f;

    // Card height that fits the scene at full card width.
    int ExampleHeight(int example, int width)
    {
        const Bounds b = ExampleBounds(example);
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

    void DrawExample(HDC dc, const RECT& card, int example)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        Gdiplus::GraphicsPath bgPath;
        RoundedRect(bgPath, static_cast<float>(card.left), static_cast<float>(card.top),
                    static_cast<float>(card.right - card.left), static_cast<float>(card.bottom - card.top), static_cast<float>(Px(10)));
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 241, 244, 251));
        g.FillPath(&bgBrush, &bgPath);

        const bool scene = example == 0;
        const Figure& f = kFigures[scene ? 0 : example - 1];
        const float figDx = scene ? 62.0f : 0.0f;   // scene: the child stands right of the camera

        // Fit the scene into the card.
        const Bounds b = ExampleBounds(example);
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
        const bool lying = !scene && example >= 3;   // the floor exercises
        const float fdx = lying ? -4.0f : -3.0f, fdy = lying ? 0.0f : -1.5f;
        // Soft highlight behind the joint whose angle the step measures.
        const float ringR = 7.5f * u;
        const Gdiplus::PointF joint = F(f.ring == 1 ? f.knee : f.ring == 2 ? f.elbow : f.hip);
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
        const Gdiplus::PointF ec = F(f.head, f.eye.x, f.eye.y);
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

        const StepDef& step = Cur();
        SelectObject(dc, g_fontTitle);
        SetTextColor(dc, kTextInk);
        r = RECT{ p.left, y, p.right, y + Px(40) };
        DrawTextW(dc, T(step.title), -1, &r, DT_LEFT | DT_WORDBREAK);
        y += Px(40);

        // Checklist above the buttons: one row per exercise. Not on the
        // summary: it says the same and needs the room.
        const bool showList = step.kind != StepKind::Summary;
        const int rowH = Px(20);
        const int rows = showList ? kExerciseCount : 0;
        const int listTop = p.bottom - Px(44) - rows * rowH;
        const int textBottom = listTop - Px(10);

        // Word-wrapped paragraph at y, clipped above the checklist.
        auto para = [&](const std::wstring& text, COLORREF color) {
            SetTextColor(dc, color);
            RECT pr{ p.left, y, p.right, textBottom };
            RECT measure = pr;
            DrawTextW(dc, text.c_str(), -1, &measure, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
            DrawTextW(dc, text.c_str(), -1, &pr, DT_LEFT | DT_WORDBREAK);
            y = std::min(measure.bottom, pr.bottom) + Px(12);
        };

        SelectObject(dc, g_fontUi);
        std::wstring body = T(step.text);
        if (step.kind == StepKind::Summary) body += L"\n\n" + SummaryText();
        para(body, kTextInk);
        if (!g_status.empty()) para(g_status, g_statusIsError ? kErrInk : kOkInk);
        if (step.kind == StepKind::Reps)
            for (const Para& t : RepsParas(step.exercise)) para(t.text, t.color);

        // Example picture in whatever room is left above the checklist.
        if (step.example >= 0 && textBottom - y >= Px(120))
        {
            SetTextColor(dc, kInfoInk);
            r = RECT{ p.left, y, p.right, y + Px(20) };
            DrawTextW(dc, T(step.example == 0 ? tExampleSetup : tExampleView), -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            const int want = std::min(ExampleHeight(step.example, p.right - p.left), Px(240));
            const RECT card{ p.left, y + Px(24), p.right, std::min(textBottom, y + Px(24) + want) };
            DrawExample(dc, card, step.example);
        }

        SetTextColor(dc, kTextInk);
        y = listTop;
        for (int ex = 0; ex < kExerciseCount && showList; ++ex)
        {
            const Capture& c = g_caps[ex];
            const std::wstring name = g_uk ? kExercises[ex].nameUk : kExercises[ex].nameEn;
            const std::wstring v = c.done ? AngleText(ex, c.plan.top) + L" / " + AngleText(ex, c.plan.bottom)
                                 : c.skipped ? std::wstring(T(tSkipped))
                                 : c.reps > 0 ? Fmt(L"%d/%d", c.reps, kCalibReps) : std::wstring(T(tNotYet));
            const std::wstring line = std::wstring(c.done ? L"\x2714  " : L"\x2022  ") + name + L": " + v;
            r = RECT{ p.left, y, p.right, y + rowH };
            DrawTextW(dc, line.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += rowH;
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

    // Webcam <-> phone: other frames and other config.ini sections - nothing
    // measured on one counts for the other.
    void SwitchSource(bool phone)
    {
        bool any = false;
        for (Capture& c : g_caps)
        {
            any = any || c.done || c.reps > 0;
            c = Capture{};
        }
        g_phoneMode = phone;
        g_aspect = 0.0;
        if (any)
        {
            g_status = T(tPhoneReset);
            g_statusIsError = false;
        }
        LogF(L"calibrate: source %s", phone ? L"phone" : L"webcam");
    }

    void OnCommand(int id, int code)
    {
        switch (id)
        {
        case idCamera:
            if (code == CBN_SELCHANGE)
            {
                g_status.clear();
                const int sel = static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0));
                const bool phone = sel >= 0 && sel == g_phoneItem;
                if (phone != g_phoneMode) SwitchSource(phone);
                if (phone)
                {
                    StopCamera();
                    StartPhone();
                }
                else
                {
                    StopPhone();
                    StartCamera(sel);
                }
                // Another camera sees the child differently: count from zero.
                if (Cur().kind == StepKind::Reps && !g_caps[Cur().exercise].done) StartReps(Cur().exercise);
                UpdatePhoneUrl();
                UpdateControls();
            }
            break;
        case idBack:
            GoToStep(g_step - 1);
            break;
        case idTake:
            // "Start over": this exercise's reps from zero.
            if (Cur().kind == StepKind::Reps)
            {
                g_status.clear();
                g_autoNextAt = 0;
                g_caps[Cur().exercise] = Capture{};
                StartReps(Cur().exercise);
                UpdateControls();
            }
            break;
        case idSkip:
            if (Cur().kind == StepKind::Camera || Cur().kind == StepKind::Summary)
            {
                OpenSettings();
                break;
            }
            g_caps[Cur().exercise] = Capture{};
            g_caps[Cur().exercise].skipped = true;
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
        // The reps gave the thresholds: on to the next exercise.
        if (g_autoNextAt && GetTickCount64() >= g_autoNextAt)
        {
            g_autoNextAt = 0;
            if (Cur().kind == StepKind::Reps && g_caps[Cur().exercise].done)
            {
                LogF(L"calibrate: %s done, next step", kExercises[Cur().exercise].logName);
                GoToStep(g_step + 1);
            }
        }
        // The relay drops a connection that says nothing for a minute.
        if (g_phone && GetTickCount64() - g_phonePingMs >= 20000)
        {
            g_phonePingMs = GetTickCount64();
            g_phone->sock.Send(R"({"t":"ping"})");
        }
        // The phone's poses start or stop: "Start over" follows.
        if (SourceReady() != g_sourceReady)
        {
            g_sourceReady = !g_sourceReady;
            UpdateControls();
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
            g_url = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_READONLY | ES_CENTER | ES_AUTOHSCROLL,
                                    0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(idUrl)), g_inst, nullptr);
            SetWindowTextW(g_back, T(tBack));
            SetWindowTextW(g_skip, T(tSkip));
            MakeFonts();

            g_cameras = EnumerateCameras();
            for (const CameraInfo& c : g_cameras)
                SendMessageW(g_cam, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.name.c_str()));
            // The phone, last in the list - when config.ini has a relay for it.
            if (Relay::RelayUrl relay; Config::PhoneEnabled() && Relay::ParseRelay(Config::PhoneRelayUrl(), relay))
                g_phoneItem = static_cast<int>(SendMessageW(g_cam, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(tPhone))));
            if (g_startPhone && g_phoneItem >= 0 && g_snapshotPath.empty())
            {
                SendMessageW(g_cam, CB_SETCURSEL, static_cast<WPARAM>(g_phoneItem), 0);
                g_phoneMode = true;
                StartPhone();
            }
            else if (!g_cameras.empty())
            {
                const int pick = std::max(PreferredCamera(), 0);
                SendMessageW(g_cam, CB_SETCURSEL, static_cast<WPARAM>(pick), 0);
                if (g_snapshotPath.empty()) StartCamera(pick);
            }
            else if (g_phoneItem >= 0 && g_snapshotPath.empty())
            {
                // No webcam at all: the phone is the camera.
                SendMessageW(g_cam, CB_SETCURSEL, static_cast<WPARAM>(g_phoneItem), 0);
                g_phoneMode = true;
                StartPhone();
            }
            if (!g_poseOk) { g_status = T(tNoModel); g_statusIsError = true; }
            Layout();
            if (!g_snapshotPath.empty())
            {
                // Layout check without a camera: the exercises before this
                // step done with sample reps, this one at 3 of 5.
                g_snapshotStep = std::clamp(g_snapshotStep, 0, kSteps - 1);
                const ExerciseUnlock::Calib::Thresholds sample[kExerciseCount] = {
                    { 162, 152, 129, 117, 0.04f }, { 161, 156, 147, 141, 0.0f }, { 121, 110, 78, 63, 0.0f } };
                const int sampleTop[kExerciseCount] = { 168, 164, 128 }, sampleBottom[kExerciseCount] = { 111, 138, 56 };
                for (int i = 0; i < g_snapshotStep; ++i)
                {
                    const StepDef& s = kStepDefs[i];
                    if (s.kind != StepKind::Reps) continue;
                    Capture& c = g_caps[s.exercise];
                    c.done = true;
                    c.plan.t = sample[s.exercise];
                    c.plan.top = sampleTop[s.exercise];
                    c.plan.bottom = sampleBottom[s.exercise];
                }
                GoToStep(g_snapshotStep);
                if (Cur().kind == StepKind::Reps)
                {
                    Capture& c = g_caps[Cur().exercise];
                    c.reps = 3;
                    c.postureOkMs = GetTickCount64();   // no "not recognized" hint in the shot
                }
                UpdateControls();
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
        case WM_APP_PHONE:
            g_calibSent.clear();   // a phone that just connected gets the step
            UpdatePhoneUrl();
            UpdateControls();
            return 0;
        case WM_APP_PHONENAV:
        {
            // A wizard button on the phone does what the computer's does, while the computer's is on.
            const int id = static_cast<int>(wp);
            const StepKind kind = Cur().kind;
            if (g_autoNextAt) return 0;
            if (id == idSkip && (kind == StepKind::Camera || kind == StepKind::Summary)) return 0;   // the settings
            const HWND b = id == idBack ? g_back : id == idNext ? g_next : id == idSkip ? g_skip : g_take;
            if (!IsWindowVisible(b) || !IsWindowEnabled(b)) return 0;
            LogF(L"calibrate: the phone pressed %d on step %d", id, g_step);
            OnCommand(id, BN_CLICKED);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lp) == g_url)
            {
                // The address sits on the dark video area.
                static HBRUSH dark = CreateSolidBrush(RGB(30, 30, 40));
                SetTextColor(reinterpret_cast<HDC>(wp), RGB(255, 230, 90));
                SetBkColor(reinterpret_cast<HDC>(wp), RGB(30, 30, 40));
                return reinterpret_cast<LRESULT>(dark);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_APP_SNAPSHOT:
            ExerciseUnlock::Calib::WriteWindowBmp(hwnd, g_snapshotPath, [](HDC dc, const RECT& rc) {
                HBRUSH bg = CreateSolidBrush(RGB(244, 246, 252));
                FillRect(dc, &rc, bg);
                DeleteObject(bg);
                PaintVideo(dc, VideoRect(rc));
                PaintPanel(dc, PanelRect(rc));
            });
            DestroyWindow(hwnd);
            return 0;
        case WM_APP_CAMERR:
            g_status = T(tCamBusy);
            g_statusIsError = true;
            UpdateControls();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            StopPhone();
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

    // The language: /lang, else the one setup ran in (like the update
    // window), else Windows'. Started without /lang - pinned to the taskbar,
    // from the search or Explorer - it used to follow Windows only, and an
    // English Windows showed English after a Ukrainian setup.
    const wchar_t* langFrom = L"windows";
    {
        wchar_t lang[16]{};
        DWORD cb = sizeof(lang);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ExerciseUnlock", L"Language", RRF_RT_REG_SZ,
                         nullptr, lang, &cb) == ERROR_SUCCESS)
        {
            g_uk = _wcsicmp(lang, L"uk") == 0;
            langFrom = L"setup";
        }
        else
            g_uk = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_UKRAINIAN;
    }
    bool settingsOnly = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"/setup") == 0) g_setupMode = true;
            else if (_wcsicmp(argv[i], L"/settings") == 0) settingsOnly = true;
            else if (_wcsicmp(argv[i], L"/phone") == 0) g_startPhone = true;
            else if (_wcsicmp(argv[i], L"/lang") == 0 && i + 1 < argc)
            {
                g_uk = _wcsicmp(argv[++i], L"uk") == 0;
                langFrom = L"/lang";
            }
            else if (_wcsicmp(argv[i], L"/snapshot") == 0 && i + 1 < argc) g_snapshotPath = argv[++i];
            else if (_wcsicmp(argv[i], L"/users") == 0 && i + 1 < argc) g_usersPath = argv[++i];
            else if (_wcsicmp(argv[i], L"/step") == 0 && i + 1 < argc) g_snapshotStep = _wtoi(argv[++i]);
        }
        LocalFree(argv);
    }

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    g_exeDir = exe;
    g_configPath = g_exeDir + L"\\config.ini";
    // The reps steps run the service's detectors: the same config.ini (the
    // service reads the one next to its exe, which is this folder), any
    // time of day.
    Config::Load();
    Config::SetIgnoreSchedule(true);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    if (settingsOnly)
    {
        g_cameras = EnumerateCameras();
        ExerciseUnlock::Calib::SettingsRequest req = SettingsFor(nullptr);
        req.snapshotPath = g_snapshotPath;
        req.snapshotPage = g_snapshotStep;
        const auto r = ExerciseUnlock::Calib::ShowSettings(req);
        if (r.serviceConfig && !g_setupMode) RestartService();
        MFShutdown();
        CoUninitialize();
        return r.saved ? 0 : 1;
    }

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);   // example pictures
    g_poseOk = SUCCEEDED(g_pose.Load(g_exeDir + L"\\movenet_lightning.onnx"));
    LogF(L"calibrate: start lang=%s (%s) setup=%d pose=%d", g_uk ? L"uk" : L"en", langFrom, g_setupMode ? 1 : 0, g_poseOk ? 1 : 0);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ExerciseUnlockCalibrate";
    // App icon from our own resources (app.rc).
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
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
