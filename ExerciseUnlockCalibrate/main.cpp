// ExerciseUnlockCalibrate — measures the child's knee / elbow angles from
// four photos (standing, squat, push-up up, push-up down) and writes the
// rep-detection thresholds into config.ini. After each exercise's photos
// the child does 5 test reps, counted by the service's own detectors with
// the new (not yet saved) thresholds.
//
//   ExerciseUnlockCalibrate.exe [/lang en|uk] [/setup] [/settings]
//
//   /setup     launched by the installer before the service starts: after
//              saving, don't restart the service (setup starts it).
//   /settings  only the settings window (Settings.cpp), no wizard and no
//              camera.
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
using ExerciseUnlock::Svc::Config;
using ExerciseUnlock::Svc::Joint;
using ExerciseUnlock::Svc::LogF;
using ExerciseUnlock::Svc::PoseEstimator;
using ExerciseUnlock::Svc::PostureClassifier;
using ExerciseUnlock::Svc::PushUpDetector;
using ExerciseUnlock::Svc::RepResult;
using ExerciseUnlock::Svc::SitUpDetector;
using ExerciseUnlock::Svc::SquatDetector;
using ExerciseUnlock::Calib::KeepOrder;
using ExerciseUnlock::Calib::Swing;
namespace Ipc = ExerciseUnlock::Ipc;

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
        tTitleTestSquat, tTextTestSquat, tTitleTestPush, tTextTestPush, tRestart,
        tTestThrSquat, tTestThrPush, tTestCounted, tTestPassed,
        tTestNotDeepSquat, tTestNotDeepPush, tTestTooFast, tTestTooSlow, tTestShallow, tTestMoved, tTestWobble, tTestHips,
        tTestNoSquat, tTestNoPlank, tTestStraight,
        tSumTestOk, tSumTestWarn, tSumTestSkipped,
        tRetakePhotos, tTestAdjusted, tAdjDown, tAdjStand, tAdjUp, tAdjHip, tTestPace, tTestForm,
        tRestartRange, tRestartStill, tRestartHips, tRestartAsk, tSumAdjusted,
        tTitleSitLying, tTextSitLying, tTitleSitUp, tTextSitUp, tTitleTestSitup, tTextTestSitup,
        tRow5, tRow6, tHip, tSitNotSeen, tSmallSitup, tSumSitup, tTestThrSitup, tTestNotDeepSitup,
        tTestNoSitup, tAdjLying, tAdjSitTop,
        tSettings, tSettingsSaved,
        tCount
    };

    const wchar_t* const kEn[] = {
        L"Exercise Unlock - calibration", L"Camera:", L"Back", L"Next", L"Take photo", L"Retake", L"Skip", L"Save", L"Close",
        L"Step %d of %d",
        L"Camera",
        L"Put the camera where the child will exercise and pick it in the list above.\n\n"
        L"The child must be fully visible from the side - from head to feet. For push-ups and sit-ups the camera has to stand on the floor.\n\n"
        L"Next you will take 6 photos: standing and squatting, push-up with straight arms and at the bottom, lying and sitting up for sit-ups. "
        L"Exercise Unlock measures the child's angles and tunes the rep counting to them; after each exercise "
        L"the child does 5 test reps to check it.\n\n"
        L"The seconds per rep, the schedule and everything else are under \"Settings\".",
        L"Standing, side view",
        L"The child stands sideways to the camera at full height: legs straight, arms down.\n\n"
        L"Press \"Take photo\" - the photo is taken right away.",
        L"Squat, side view",
        L"The child squats as deep as in a normal rep and holds still, still sideways to the camera.\n\n"
        L"Press \"Take photo\" and keep the squat until the photo appears.",
        L"Push-up, arms straight",
        L"Put the camera on the floor, 2-3 m away. The child takes the plank position sideways to the camera, arms straight.\n\n"
        L"Press \"Take photo\".",
        L"Push-up, bottom",
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
        L"Squat check",
        L"The child does 5 squats at the usual pace, sideways to the camera - just like on the lock screen.\n\n"
        L"The reps are counted with the new settings exactly the way the lock screen will count them. "
        L"If a rep is not counted, the reason is shown below.",
        L"Push-up check",
        L"The child does 5 push-ups at the usual pace, sideways to the camera - just like on the lock screen.\n\n"
        L"The reps are counted with the new settings exactly the way the lock screen will count them. "
        L"If a rep is not counted, the reason is shown below.",
        L"Start over",
        L"A rep counts when the knee bends to %d\x00B0 or less and straightens back to %d\x00B0 or more.",
        L"A rep counts when the elbow bends to %d\x00B0 or less and straightens back to %d\x00B0 or more.",
        L"Counted: %d of %d.",
        L"Check passed: %d of %d reps counted. Press \"Next\".",
        L"Last rep not counted: not deep enough - the knee bent to %d\x00B0, it needs %d\x00B0 or less.",
        L"Last rep not counted: not low enough - the elbow bent to %d\x00B0, it needs %d\x00B0 or less.",
        L"Last rep not counted: too fast (%.1f s, at least %.1f s needed).",
        L"Last rep not counted: too slow (%.1f s, at most %.1f s).",
        L"Last rep not counted: not deep enough.",
        L"Last rep not counted: the child moved from the spot.",
        L"Last rep not counted: the movement went down again on the way up - stand up in one go.",
        L"Last rep not counted: the hips did not go down - squat, don't just lift a knee.",
        L"No squat position recognized: the child must be fully visible, from head to feet.",
        L"No push-up position recognized: the whole body must be visible, hands on the floor.",
        L"Keep the body straight, as in a plank.",
        L"\x2714 Check passed: %d of %d reps counted",
        L"\x26A0 Check not finished (%d of %d) - reps may not count. Retake the photos or run the check again.",
        L"Check skipped",
        L"Retake",
        L"The thresholds were adjusted to the child's test reps: %s. Do 5 reps again.",
        L"down %d\x00B0 \x2192 %d\x00B0",
        L"standing %d\x00B0 \x2192 %d\x00B0",
        L"up %d\x00B0 \x2192 %d\x00B0",
        L"hip drop %.2f \x2192 %.2f",
        L"The reps were not counted because of the pace: one rep should take %.1f-%.1f s. Do 5 reps again at a steady pace.",
        L"The reps were not counted because of the movement (see the reason above). Do 5 reps again.",
        L"The test reps differ too much from the photos (only %d\x00B0 between top and bottom).",
        L"The check failed several times in a row.",
        L"The hips hardly move in the picture - the camera probably sees the child from a bad angle.",
        L"Calibrate this exercise again: press \"Retake\" and take its photos once more.",
        L"Thresholds adjusted to the test reps",
        L"Sit-up, lying",
        L"The child lies on the back sideways to the camera: knees bent, feet flat on the floor, arms crossed on the chest. "
        L"The camera stays on the floor, 2-3 m away.\n\n"
        L"Press \"Take photo\".",
        L"Sit-up, top",
        L"The child sits up as in a normal rep - as high as usual - and holds still.\n\n"
        L"Press \"Take photo\" and keep the position until the photo appears.",
        L"Sit-up check",
        L"The child does 5 sit-ups at the usual pace, sideways to the camera - just like on the lock screen.\n\n"
        L"The reps are counted with the new settings exactly the way the lock screen will count them. "
        L"If a rep is not counted, the reason is shown below.",
        L"Sit-up lying", L"Sit-up top", L"hip %d\x00B0",
        L"No sit-up position recognized: shoulders, hips, knees and ankles must be visible, the child on the back with the knees bent. Put the camera on the floor and try again.",
        L"Lying and sitting up differ by only %d\x00B0 - sit up higher or check the camera angle. Sit-up settings were not changed.",
        L"Sit-ups (hip angle)\n  lying %s \x2192 %d, rising %s \x2192 %d\n  lowering %s \x2192 %d, top %s \x2192 %d",
        L"A rep counts when the hip angle closes to %d\x00B0 or less and opens back to %d\x00B0 or more.",
        L"Last rep not counted: not high enough - the hip angle closed to %d\x00B0, it needs %d\x00B0 or less.",
        L"No sit-up position recognized: the child lies on the back sideways to the camera, knees bent, the whole body visible.",
        L"lying %d\x00B0 \x2192 %d\x00B0",
        L"top %d\x00B0 \x2192 %d\x00B0",
        L"Settings",
        L"Settings saved.",
    };

    const wchar_t* const kUk[] = {
        L"Exercise Unlock - калібрування", L"Камера:", L"Назад", L"Далі", L"Зробити фото", L"Перезняти", L"Пропустити", L"Зберегти", L"Закрити",
        L"Крок %d з %d",
        L"Камера",
        L"Поставте камеру там, де дитина виконуватиме вправи, і виберіть її у списку вище.\n\n"
        L"Дитину має бути видно збоку повністю - від голови до ніг. Для віджимань і преса камера має стояти на підлозі.\n\n"
        L"Далі ви зробите 6 фото: стоячи й у присіді, віджимання з прямими руками й унизу, лежачи й угорі для преса. "
        L"Exercise Unlock виміряє кути дитини й налаштує під них підрахунок повторів, а після кожної вправи "
        L"дитина зробить 5 пробних повторів для перевірки.\n\n"
        L"Секунди за повтор, розклад та все інше - у «Налаштуваннях».",
        L"Стоячи, у профіль",
        L"Дитина стоїть боком до камери на повний зріст: ноги прямі, руки опущені.\n\n"
        L"Натисніть «Зробити фото» - знімок буде зроблено одразу.",
        L"Присід, у профіль",
        L"Дитина присідає так глибоко, як у звичайному повторенні, і завмирає - так само боком до камери.\n\n"
        L"Натисніть «Зробити фото» й тримайте присід, доки не з'явиться знімок.",
        L"Віджимання, руки прямі",
        L"Поставте камеру на підлогу за 2-3 м. Дитина стає в планку боком до камери, руки прямі.\n\n"
        L"Натисніть «Зробити фото».",
        L"Віджимання, внизу",
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
        L"Перевірка присідань",
        L"Дитина робить 5 присідань у звичному темпі, боком до камери, - так само, як на екрані блокування.\n\n"
        L"Повтори рахуються з новими налаштуваннями точно так, як їх рахуватиме екран блокування. "
        L"Якщо повтор не зараховано, нижче буде вказано причину.",
        L"Перевірка віджимань",
        L"Дитина робить 5 віджимань у звичному темпі, боком до камери, - так само, як на екрані блокування.\n\n"
        L"Повтори рахуються з новими налаштуваннями точно так, як їх рахуватиме екран блокування. "
        L"Якщо повтор не зараховано, нижче буде вказано причину.",
        L"Почати знову",
        L"Повтор зараховується, коли коліно згинається до %d\x00B0 або менше й випрямляється до %d\x00B0 або більше.",
        L"Повтор зараховується, коли лікоть згинається до %d\x00B0 або менше й випрямляється до %d\x00B0 або більше.",
        L"Зараховано: %d з %d.",
        L"Перевірку пройдено: зараховано %d з %d повторів. Натисніть «Далі».",
        L"Останній повтор не зараховано: замало глибоко - коліно зігнулося до %d\x00B0, потрібно %d\x00B0 або менше.",
        L"Останній повтор не зараховано: замало низько - лікоть зігнувся до %d\x00B0, потрібно %d\x00B0 або менше.",
        L"Останній повтор не зараховано: занадто швидко (%.1f с, потрібно щонайменше %.1f с).",
        L"Останній повтор не зараховано: занадто повільно (%.1f с, максимум %.1f с).",
        L"Останній повтор не зараховано: замало глибоко.",
        L"Останній повтор не зараховано: дитина зрушила з місця.",
        L"Останній повтор не зараховано: на підйомі рух знову пішов униз - вставайте за один рух.",
        L"Останній повтор не зараховано: стегна не опустилися - треба присідати, а не піднімати коліно.",
        L"Присід не розпізнано: дитину має бути видно повністю, від голови до ніг.",
        L"Віджимання не розпізнано: має бути видно все тіло, руки - на підлозі.",
        L"Тримайте тіло рівно, як у планці.",
        L"\x2714 Перевірку пройдено: зараховано %d з %d",
        L"\x26A0 Перевірку не завершено (%d з %d) - повтори можуть не зараховуватися. Перезніміть фото або повторіть перевірку.",
        L"Перевірку пропущено",
        L"Перезняти",
        L"Пороги скориговано за пробними повторами дитини: %s. Зробіть 5 повторів ще раз.",
        L"низ %d\x00B0 \x2192 %d\x00B0",
        L"стійка %d\x00B0 \x2192 %d\x00B0",
        L"угорі %d\x00B0 \x2192 %d\x00B0",
        L"опускання стегон %.2f \x2192 %.2f",
        L"Повтори не зараховано через темп: один повтор має тривати %.1f-%.1f с. Зробіть 5 повторів ще раз у рівному темпі.",
        L"Повтори не зараховано через рух (причину вказано вище). Зробіть 5 повторів ще раз.",
        L"Пробні повтори надто відрізняються від фото (між верхом і низом лише %d\x00B0).",
        L"Перевірка кілька разів поспіль не вдалася.",
        L"Стегна майже не рухаються в кадрі - мабуть, камера бачить дитину під невдалим кутом.",
        L"Відкалібруйте цю вправу заново: натисніть «Перезняти» й зробіть її фото ще раз.",
        L"Пороги скориговано за пробними повторами",
        L"Прес, лежачи",
        L"Дитина лежить на спині боком до камери: коліна зігнуті, стопи на підлозі, руки схрещені на грудях. "
        L"Камера так само на підлозі за 2-3 м.\n\n"
        L"Натисніть «Зробити фото».",
        L"Прес, угорі",
        L"Дитина піднімає корпус, як у звичайному повторенні, - так само високо - і завмирає.\n\n"
        L"Натисніть «Зробити фото» й тримайте положення, доки не з'явиться знімок.",
        L"Перевірка преса",
        L"Дитина робить 5 підйомів корпусу у звичному темпі, боком до камери, - так само, як на екрані блокування.\n\n"
        L"Повтори рахуються з новими налаштуваннями точно так, як їх рахуватиме екран блокування. "
        L"Якщо повтор не зараховано, нижче буде вказано причину.",
        L"Прес лежачи", L"Прес угорі", L"стегно %d\x00B0",
        L"Положення для преса не розпізнано: мають бути видні плечі, стегна, коліна й щиколотки, дитина - на спині із зігнутими колінами. Поставте камеру на підлогу й спробуйте ще раз.",
        L"Лежачи й угорі кут відрізняється лише на %d\x00B0 - підніміться вище або перевірте ракурс камери. Налаштування преса не змінено.",
        L"Прес (кут у стегні)\n  лежачи %s \x2192 %d, підйом %s \x2192 %d\n  опускання %s \x2192 %d, угорі %s \x2192 %d",
        L"Повтор зараховується, коли кут у стегні зменшується до %d\x00B0 або менше й повертається до %d\x00B0 або більше.",
        L"Останній повтор не зараховано: замало високо - кут у стегні зменшився до %d\x00B0, потрібно %d\x00B0 або менше.",
        L"Положення для преса не розпізнано: дитина лежить на спині боком до камери, коліна зігнуті, усе тіло в кадрі.",
        L"лежачи %d\x00B0 \x2192 %d\x00B0",
        L"угорі %d\x00B0 \x2192 %d\x00B0",
        L"Налаштування",
        L"Налаштування збережено.",
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

    // Hip angle (shoulder-hip-knee) the way SitUpDetector sees it: average
    // of the confident sides - or -1 unless the legs are in the sit-up
    // shape PostureClassifier latches on (knees above hips and feet, feet
    // level with the hips and out in front).
    constexpr float kSitupConf = 0.20f;   // SitUpDetector / PostureClassifier
    float SitupHip(const BodyPose& p)
    {
        float sum = 0.0f;
        int n = 0;
        if (Conf3(p.joints[kLSh], p.joints[kLHip], p.joints[kLKn], kSitupConf))
        { sum += AngleAt(p.joints[kLSh], p.joints[kLHip], p.joints[kLKn]); ++n; }
        if (Conf3(p.joints[kRSh], p.joints[kRHip], p.joints[kRKn], kSitupConf))
        { sum += AngleAt(p.joints[kRSh], p.joints[kRHip], p.joints[kRKn]); ++n; }
        if (n == 0) return -1.0f;

        auto avg = [&](int l, int r, float& x, float& y) {
            int k = 0;
            x = y = 0.0f;
            for (int i : { l, r })
                if (p.joints[i].confidence >= kSitupConf) { x += p.joints[i].x; y += p.joints[i].y; ++k; }
            if (k) { x /= static_cast<float>(k); y /= static_cast<float>(k); }
            return k > 0;
        };
        float hx, hy, kx, ky, ax, ay;
        if (!avg(kLHip, kRHip, hx, hy) || !avg(kLKn, kRKn, kx, ky) || !avg(kLAn, kRAn, ax, ay)) return -1.0f;
        const float thigh = std::hypot(hx - kx, hy - ky);
        if (thigh <= 0.03f) return -1.0f;
        const bool shape = std::min(hy, ay) - ky >= 0.30f * thigh &&
                           std::fabs(ay - hy) <= 0.45f * thigh &&
                           std::fabs(ax - hx) >= 0.50f * thigh;
        return shape ? sum / static_cast<float>(n) : -1.0f;
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
        float angle = -1.0f;   // knee (photos 1-2), elbow (3-4) or hip (5-6)
        float hipY  = -1.0f;   // photos 1-2
        Frame photo;
    };

    struct CameraInfo { std::wstring name, link; };

    // "Take photo" measures straight away: the parent presses it while the
    // child holds the pose, so there is no countdown.
    enum class Phase { Live, Collecting };

    // Each exercise: its photos, then a test with the new thresholds. A new
    // exercise adds its rows here, an ExerciseInfo, and its Plan / Save
    // lines and detector.
    enum class StepKind { Camera, Photo, Test, Summary };
    enum Exercise { kSquat = 0, kPushUp = 1, kSitUp = 2, kExerciseCount };
    struct StepDef
    {
        StepKind kind;
        int      exercise;   // Photo / Test: an Exercise, else -1
        int      shot;       // Photo: index into g_shots (1..6), else 0
        int      example;    // example picture: 0 camera scene, 1..6 a figure, -1 none
        Txt      title, text;
    };
    constexpr StepDef kStepDefs[] = {
        { StepKind::Camera,  -1,      0,  0, tTitle0,         tText0 },
        { StepKind::Photo,   kSquat,  1,  1, tTitle1,         tText1 },
        { StepKind::Photo,   kSquat,  2,  2, tTitle2,         tText2 },
        { StepKind::Test,    kSquat,  0, -1, tTitleTestSquat, tTextTestSquat },
        { StepKind::Photo,   kPushUp, 3,  3, tTitle3,         tText3 },
        { StepKind::Photo,   kPushUp, 4,  4, tTitle4,         tText4 },
        { StepKind::Test,    kPushUp, 0, -1, tTitleTestPush,  tTextTestPush },
        { StepKind::Photo,   kSitUp,  5,  5, tTitleSitLying,  tTextSitLying },
        { StepKind::Photo,   kSitUp,  6,  6, tTitleSitUp,     tTextSitUp },
        { StepKind::Test,    kSitUp,  0, -1, tTitleTestSitup, tTextTestSitup },
        { StepKind::Summary, -1,      0, -1, tTitle5,         tText5 },
    };
    constexpr int kShotCount = 7;   // g_shots[1..6]

    // Per-exercise texts, measurement and detector bits.
    struct ExerciseInfo
    {
        const wchar_t* logName;
        Txt   rowTop, rowDown;   // checklist labels of its two photos
        Txt   angle;             // "knee %d°" ...
        Txt   notSeen;           // a photo found no such pose
        Txt   tooClose;          // the two photos too close
        Txt   summary;           // summary block
        Txt   threshold;         // test: "a rep counts when ..."
        Txt   notDeep;           // test: last rep didn't reach "down"
        Txt   noPose;            // test: pose not recognized
        Txt   adjTop, adjDown;   // adjustment note pieces
        Ipc::PostureKind posture;
        const wchar_t* nameEn;
        const wchar_t* nameUk;
        float (*measure)(const BodyPose&);
    };
    const ExerciseInfo kExercises[kExerciseCount] = {
        { L"squat",  tRow1, tRow2, tKnee,  tLegsNotSeen, tSmallSquat, tSumSquat, tTestThrSquat, tTestNotDeepSquat,
          tTestNoSquat, tAdjStand, tAdjDown, Ipc::PostureKind::Squat, L"Squats", L"Присідання", KneeAngle },
        { L"pushup", tRow3, tRow4, tElbow, tPushNotSeen, tSmallPush, tSumPush, tTestThrPush, tTestNotDeepPush,
          tTestNoPlank, tAdjUp, tAdjDown, Ipc::PostureKind::PushUp, L"Push-ups", L"Віджимання", PushUpElbow },
        { L"situp",  tRow5, tRow6, tHip,   tSitNotSeen, tSmallSitup, tSumSitup, tTestThrSitup, tTestNotDeepSitup,
          tTestNoSitup, tAdjLying, tAdjSitTop, Ipc::PostureKind::SitUp, L"Sit-ups", L"Прес", SitupHip },
    };
    constexpr int kSteps = static_cast<int>(std::size(kStepDefs));
    constexpr int kTestReps = 5;
    constexpr UINT WM_APP_FRAME  = WM_APP + 1;
    constexpr UINT WM_APP_CAMERR = WM_APP + 2;
    constexpr UINT_PTR kTimerId  = 1;
    constexpr DWORD kCollectMs   = 5000;
    constexpr size_t kSamples    = 12;

    enum CtlId { idCamera = 100, idBack, idTake, idSkip, idNext };

    HINSTANCE g_inst = nullptr;
    HWND g_wnd = nullptr, g_cam = nullptr, g_back = nullptr, g_take = nullptr, g_skip = nullptr, g_next = nullptr;
    HFONT g_fontUi = nullptr, g_fontTitle = nullptr, g_fontBig = nullptr;
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
    Shot g_shots[kShotCount];
    std::wstring g_status;            // error / info line under the instructions
    bool g_statusIsError = false;
    bool g_saved = false;

    const StepDef& Cur() { return kStepDefs[g_step]; }

    // ------------------------------------------------------------ test reps
    // While a test step is open the camera thread runs the service's
    // detectors on every frame (TestFrame) and publishes what they saw;
    // the UI thread only reads that.

    struct TestFeed
    {
        uint32_t  gen = 0;
        uint32_t  count[kExerciseCount]{};
        float     angle[kExerciseCount]{};
        uint8_t   posture = 0;
        uint8_t   pushPhase = 0;
        RepResult last[kExerciseCount]{};
        std::vector<float> peaks[kExerciseCount], valleys[kExerciseCount];
    };
    std::atomic<int>      g_testExercise{ -1 };   // detectors run for this one; -1 = off
    std::atomic<uint32_t> g_testGen{ 0 };         // bumped = detectors start from zero
    std::mutex            g_testMx;
    TestFeed              g_testFeed;             // under g_testMx

    struct TestRun
    {
        bool      passed = false, skipped = false;
        int       counted = 0;
        uint32_t  lastSeq = 0;
        RepResult last{};                  // seq 0 = no finished attempt yet
        std::vector<RepResult> attempts;   // every finished attempt of this run
        std::vector<float> peaks, valleys; // Swing turning points of this run
        ULONGLONG lastValleyMs = 0;
        float     angle = 0.0f;
        bool      notInPlank = false;
        ULONGLONG postureOkMs = 0;         // last frame the exercise's pose was recognized
    };
    TestRun g_tests[kExerciseCount];
    float g_cfgMinHipDrop = 0.0f;          // config.ini's, for plans without a hip drop

    // What a failed check changed: thresholds loosened to fit the test
    // reps (ComputePlan uses them instead of the photo-based ones), or a
    // request for new photos. Lasts until the exercise's photos change.
    struct Fix
    {
        bool  on = false;
        int   st = 0, gd = 0, gu = 0, dn = 0;
        bool  hip = false;
        float hipDrop = 0.0f;
        int   rounds = 0;                  // adjustments made so far
        bool  restart = false;             // new photos needed
        std::wstring note;                 // why / what changed, shown on the test step
    };
    Fix g_fix[kExerciseCount];

    // The exercise's photos changed (or are about to): earlier checks and
    // adjustments no longer apply.
    void ResetExercise(int ex)
    {
        g_tests[ex] = TestRun{};
        g_fix[ex] = Fix{};
    }

    // The camera thread's own detectors; same frame order as the
    // service's StreamPose.
    struct TestDetectors
    {
        SquatDetector     squats;
        PushUpDetector    pushups;
        SitUpDetector     situps;
        PostureClassifier posture;
        Swing             swing[kExerciseCount];
        uint32_t          gen = 0;
        bool              started = false;
    };

    void TestFrame(TestDetectors& d, const BodyPose& pose)
    {
        if (g_testExercise.load() < 0) return;
        const uint32_t gen = g_testGen.load();
        if (!d.started || d.gen != gen)
        {
            d.squats.Reset();
            d.pushups.Reset();
            d.situps.Reset();
            d.posture.Reset();
            for (Swing& s : d.swing) s.Reset();
            d.swing[kPushUp].hyst = 8.0f;   // MoveNet reads push-up elbows with a small span
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
        const uint8_t posture = st.postureKind.load();
        for (int ex = 0; ex < kExerciseCount; ++ex)
            if (posture == static_cast<uint8_t>(kExercises[ex].posture)) d.swing[ex].Push(angle[ex]);

        std::lock_guard<std::mutex> lk(g_testMx);
        for (int ex = 0; ex < kExerciseCount; ++ex)
        {
            g_testFeed.peaks[ex] = d.swing[ex].peaks;
            g_testFeed.valleys[ex] = d.swing[ex].valleys;
            g_testFeed.angle[ex] = angle[ex];
        }
        g_testFeed.gen = gen;
        g_testFeed.count[kSquat] = st.squatCount.load();
        g_testFeed.count[kPushUp] = st.pushUpCount.load();
        g_testFeed.count[kSitUp] = st.sitUpCount.load();
        g_testFeed.posture = posture;
        g_testFeed.pushPhase = st.pushUpPhase.load();
        g_testFeed.last[kSquat] = d.squats.LastRep();
        g_testFeed.last[kPushUp] = d.pushups.LastRep();
        g_testFeed.last[kSitUp] = d.situps.LastRep();
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
        TestDetectors test;
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
                            TestFrame(test, pose);
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
    // One exercise's four thresholds, as the detectors name them: top (rest
    // pose: standing / arms straight / lying), going down, going up, down
    // (squatting / bottom of the push-up / sitting up).
    struct ExercisePlan
    {
        bool ok = false;
        int top = 0, goingDown = 0, goingUp = 0, down = 0;
        std::wstring why;   // set when the two photos are too close
    };
    struct Plan
    {
        ExercisePlan ex[kExerciseCount];
        bool  hip = false;       // squat: a hip-drop gate from the photos
        float hipDrop = 0.0f;
    };

    // Where each exercise's thresholds live in config.ini, in the order above.
    struct IniKeys { const wchar_t* section; const wchar_t* key[4]; };
    const IniKeys kIni[kExerciseCount] = {
        { L"squat",  { L"standing_angle", L"going_down_angle", L"going_up_angle", L"down_angle" } },
        { L"pushup", { L"up_angle",       L"going_down_angle", L"going_up_angle", L"down_angle" } },
        { L"situp",  { L"lying_angle",    L"rising_angle",     L"lowering_angle", L"sitting_angle" } },
    };

    // The exercise's two photos: the rest pose first, then the other end.
    void PhotoShots(int ex, int& topShot, int& downShot)
    {
        topShot = downShot = 0;
        for (const StepDef& s : kStepDefs)
            if (s.kind == StepKind::Photo && s.exercise == ex) (topShot ? downShot : topShot) = s.shot;
    }

    // The photos are held poses, and a held squat is deeper than a rep
    // done at speed — on top of that the detector's median + EMA filters
    // shave the peak off a fast rep. So the "down" thresholds sit well
    // above the photo (45-60% of the range), the "standing" ones just
    // below it (15-25%). First field test: standing 175 / squat 76 gave
    // down=96, and no rep ever got that deep; the hand-tuned config that
    // worked for that child was 158/150/135/125. Sit-ups get the same
    // treatment (a held top is higher than a rep's). The check after the
    // photos corrects what this guess still gets wrong (CheckJudge.h).
    ExercisePlan PlanFromPhotos(int ex, float top, float bot)
    {
        ExercisePlan e;
        const float range = top - bot;
        // MoveNet reads elbows in profile with a small span (the hand-tuned
        // config used 158/150/148/142), so push-ups accept ranges from 15
        // degrees.
        const bool push = ex == kPushUp;
        if (range < (push ? 15.0f : 30.0f))
        {
            e.why = Fmt(T(kExercises[ex].tooClose), static_cast<int>(std::lround(range)));
            return e;
        }
        e.ok = true;
        if (push)
        {
            e.top       = static_cast<int>(std::lround(top - std::max(4.0f, 0.15f * range)));
            e.goingDown = static_cast<int>(std::lround(top - std::max(6.0f, 0.30f * range)));
            e.goingUp   = static_cast<int>(std::lround(bot + 0.55f * range));
            e.down      = static_cast<int>(std::lround(bot + 0.40f * range));
            KeepOrder(e.goingDown, e.goingUp, 2);
            KeepOrder(e.goingUp, e.down, 2);
        }
        else
        {
            e.top       = static_cast<int>(std::lround(top - std::max(6.0f, 0.15f * range)));
            e.goingDown = static_cast<int>(std::lround(top - std::max(10.0f, 0.25f * range)));
            e.goingUp   = static_cast<int>(std::lround(bot + 0.60f * range));
            e.down      = static_cast<int>(std::lround(bot + 0.45f * range));
            KeepOrder(e.goingDown, e.goingUp, 4);
            KeepOrder(e.goingUp, e.down, 4);
        }
        return e;
    }

    Plan ComputePlan()
    {
        Plan p;
        for (int ex = 0; ex < kExerciseCount; ++ex)
        {
            int a = 0, b = 0;
            PhotoShots(ex, a, b);
            if (g_shots[a].done && g_shots[b].done)
                p.ex[ex] = PlanFromPhotos(ex, g_shots[a].angle, g_shots[b].angle);
        }
        const Shot& s1 = g_shots[1];
        const Shot& s2 = g_shots[2];
        if (p.ex[kSquat].ok && s1.hipY > 0 && s2.hipY > s1.hipY)
        {
            // The squat detector also wants the hips to really go down;
            // ask for ~30% of the posed drop (reps are shallower than the
            // held photo, kids stand far from the camera).
            p.hip = true;
            p.hipDrop = std::clamp(std::round((s2.hipY - s1.hipY) * 0.3f * 100.0f) / 100.0f, 0.02f, 0.08f);
        }

        // Thresholds a failed check loosened to fit the test reps.
        for (int ex = 0; ex < kExerciseCount; ++ex)
        {
            const Fix& f = g_fix[ex];
            ExercisePlan& e = p.ex[ex];
            if (!e.ok || !f.on) continue;
            e.top = f.st; e.goingDown = f.gd; e.goingUp = f.gu; e.down = f.dn;
            if (ex == kSquat && f.hip) { p.hip = true; p.hipDrop = f.hipDrop; }
        }
        return p;
    }

    bool PlanReady(const Plan& p, int ex) { return p.ex[ex].ok; }

    // The squat's hip-drop gate as the test runs it: the photos' value, or
    // config.ini's when the photos gave none.
    float TestHipDrop(const Plan& p) { return p.hip ? p.hipDrop : g_cfgMinHipDrop; }

    ExerciseUnlock::Calib::Thresholds CurrentThresholds(const Plan& p, int ex)
    {
        const ExercisePlan& e = p.ex[ex];
        return { e.top, e.goingDown, e.goingUp, e.down, ex == kSquat ? TestHipDrop(p) : 0.0f };
    }

    const ExerciseUnlock::Calib::Rules& RulesOf(int ex)
    {
        using namespace ExerciseUnlock::Calib;
        return ex == kSquat ? kSquatRules : ex == kPushUp ? kPushupRules : kSitupRules;
    }

    void RepWindow(int ex, int& minMs, int& maxMs)
    {
        minMs = ex == kSquat ? Config::SquatMinRepMs() : ex == kPushUp ? Config::PushupMinRepMs() : Config::SitupMinRepMs();
        maxMs = ex == kSquat ? Config::SquatMaxRepMs() : ex == kPushUp ? Config::PushupMaxRepMs() : Config::SitupMaxRepMs();
    }

    // A test needs all of its exercise's photos.
    bool PhotosDone(int ex)
    {
        for (const StepDef& s : kStepDefs)
            if (s.kind == StepKind::Photo && s.exercise == ex && !g_shots[s.shot].done) return false;
        return true;
    }

    const wchar_t* ExerciseName(int ex) { return kExercises[ex].logName; }

    void StopTest() { g_testExercise = -1; }

    // Counts from zero with this exercise's new (unsaved) thresholds.
    void StartTest(int ex)
    {
        const Plan p = ComputePlan();
        g_tests[ex] = TestRun{};
        if (!PlanReady(p, ex) || g_fix[ex].restart)
        {
            StopTest();
            return;
        }
        const ExercisePlan& e = p.ex[ex];
        const float top = static_cast<float>(e.top), gd = static_cast<float>(e.goingDown),
                    gu = static_cast<float>(e.goingUp), dn = static_cast<float>(e.down);
        switch (ex)
        {
        case kSquat:
            Config::SetSquatAngles(top, gd, gu, dn);
            Config::SetSquatMinHipDrop(TestHipDrop(p));
            break;
        case kPushUp:
            Config::SetPushupAngles(top, gd, gu, dn);
            break;
        default:
            Config::SetSitupAngles(top, gd, gu, dn);
            break;
        }
        g_tests[ex].postureOkMs = GetTickCount64();
        ++g_testGen;
        g_testExercise = ex;
        LogF(L"calibrate: %s test started", ExerciseName(ex));
    }

    void UpdateControls();

    // ------------------------------------------------------------ check verdict
    // Three reps that did not count end a round of the check; then
    // Calib::Judge decides from the reps themselves: thresholds loosened to
    // what they really reach, a word about the pace or the movement, or new
    // photos when no thresholds would fit.
    constexpr int kFailuresPerRound = 3;
    constexpr ULONGLONG kSettleMs   = 2500;   // after the last bottom: that rep has finished by now

    void AskRestart(int ex, const std::wstring& why)
    {
        g_fix[ex].restart = true;
        g_fix[ex].note = why;
        StopTest();
        LogF(L"calibrate: %s check: new photos needed - %s", ExerciseName(ex), why.c_str());
        UpdateControls();
    }

    void JudgeTest(int ex)
    {
        using ExerciseUnlock::Calib::Verdict;
        const TestRun& run = g_tests[ex];
        Fix& fix = g_fix[ex];
        const Plan p = ComputePlan();
        const ExerciseUnlock::Calib::Thresholds cur = CurrentThresholds(p, ex);
        const auto j = ExerciseUnlock::Calib::Judge(run.attempts, run.peaks, run.valleys, cur, RulesOf(ex), fix.rounds);
        LogF(L"calibrate: %s check round %d: valleys=%zu peaks=%zu attempts=%zu reps reach %d..%d -> verdict %d",
             ExerciseName(ex), fix.rounds, run.valleys.size(), run.peaks.size(), run.attempts.size(),
             j.bottom, j.top, static_cast<int>(j.verdict));

        switch (j.verdict)
        {
        case Verdict::NewPhotosRange:    return AskRestart(ex, Fmt(T(tRestartRange), j.range));
        case Verdict::NewPhotosHips:     return AskRestart(ex, T(tRestartHips));
        case Verdict::NewPhotosRepeated: return AskRestart(ex, T(tRestartStill));
        case Verdict::Pace:
        {
            int minMs = 0, maxMs = 0;
            RepWindow(ex, minMs, maxMs);
            fix.note = Fmt(T(tTestPace), minMs / 1000.0, maxMs / 1000.0);
            break;
        }
        case Verdict::Form:
            fix.note = T(tTestForm);
            break;
        case Verdict::Retry:
            ++fix.rounds;
            fix.note = T(tTestForm);
            break;
        case Verdict::Adjust:
        {
            std::wstring what;
            auto add = [&](const std::wstring& s) { what += (what.empty() ? L"" : L", ") + s; };
            if (j.t.down != cur.down) add(Fmt(T(kExercises[ex].adjDown), cur.down, j.t.down));
            if (j.t.top != cur.top) add(Fmt(T(kExercises[ex].adjTop), cur.top, j.t.top));
            if (j.t.hipDrop < cur.hipDrop) add(Fmt(T(tAdjHip), cur.hipDrop, j.t.hipDrop));
            fix.on = true;
            fix.st = j.t.top; fix.gd = j.t.goingDown; fix.gu = j.t.goingUp; fix.dn = j.t.down;
            if (j.t.hipDrop < cur.hipDrop) { fix.hip = true; fix.hipDrop = j.t.hipDrop; }
            ++fix.rounds;
            fix.note = Fmt(T(tTestAdjusted), what.c_str());
            LogF(L"calibrate: %s thresholds adjusted: %d/%d/%d/%d hip %.2f",
                 ExerciseName(ex), fix.st, fix.gd, fix.gu, fix.dn, j.t.hipDrop);
            break;
        }
        }
        StartTest(ex);
        UpdateControls();
    }

    // UI thread, per frame: take over what the detectors published.
    void PollTest()
    {
        const int ex = Cur().exercise;
        if (Cur().kind != StepKind::Test || g_testExercise.load() != ex) return;
        TestFeed f;
        {
            std::lock_guard<std::mutex> lk(g_testMx);
            f = g_testFeed;
        }
        if (f.gen != g_testGen.load()) return;   // the previous run's numbers
        TestRun& run = g_tests[ex];
        run.counted = std::min(static_cast<int>(f.count[ex]), kTestReps);
        run.angle = f.angle[ex];
        if (f.posture == static_cast<uint8_t>(kExercises[ex].posture)) run.postureOkMs = GetTickCount64();
        run.notInPlank = ex == kPushUp && f.pushPhase == static_cast<uint8_t>(Ipc::PushUpPhase::NotInPlank);
        const RepResult& r = f.last[ex];
        if (r.seq != 0 && r.seq != run.lastSeq)
        {
            run.lastSeq = r.seq;
            run.last = r;
            run.attempts.push_back(r);
            LogF(L"calibrate: %s test attempt %u counted=%d down=%d deepest=%.1f dur=%u ms "
                 L"fast=%d slow=%d shallow=%d moved=%d wobbled=%d hips=%d hipDrop=%.2f",
                 ExerciseName(ex), r.seq, r.counted ? 1 : 0, r.reachedDown ? 1 : 0, r.deepest, r.durMs,
                 r.tooFast ? 1 : 0, r.tooSlow ? 1 : 0, r.shallow ? 1 : 0, r.moved ? 1 : 0,
                 r.wobbled ? 1 : 0, r.hipsStayed ? 1 : 0, r.hipDrop);
        }
        if (f.valleys[ex].size() > run.valleys.size()) run.lastValleyMs = GetTickCount64();
        run.peaks = f.peaks[ex];
        run.valleys = f.valleys[ex];
        if (run.counted >= kTestReps && !run.passed)
        {
            run.passed = true;
            StopTest();
            LogF(L"calibrate: %s test passed", ExerciseName(ex));
            UpdateControls();
            return;
        }
        // Every bottom is a rep the child tried; the ones not counted add up.
        const int failures = static_cast<int>(run.valleys.size()) - run.counted;
        if (failures >= kFailuresPerRound && GetTickCount64() - run.lastValleyMs >= kSettleMs)
            JudgeTest(ex);
    }

    // Why the last attempt did not count, in the parent's words.
    std::wstring WhyNotCounted(int ex, const RepResult& r, const Plan& p)
    {
        if (!r.reachedDown && r.deepest < 360.0f)
            return Fmt(T(kExercises[ex].notDeep), static_cast<int>(std::lround(r.deepest)), p.ex[ex].down);
        const double sec = r.durMs / 1000.0;
        int minMs = 0, maxMs = 0;
        RepWindow(ex, minMs, maxMs);
        if (r.tooFast) return Fmt(T(tTestTooFast), sec, minMs / 1000.0);
        if (r.tooSlow) return Fmt(T(tTestTooSlow), sec, maxMs / 1000.0);
        if (r.hipsStayed) return T(tTestHips);
        if (r.moved) return T(tTestMoved);
        if (r.wobbled) return T(tTestWobble);
        return T(tTestShallow);
    }

    struct Para { std::wstring text; COLORREF color; };
    constexpr COLORREF kInfoInk = RGB(90, 96, 130), kOkInk = RGB(20, 150, 110), kErrInk = RGB(200, 40, 60),
                       kWarnInk = RGB(200, 110, 20), kTextInk = RGB(31, 37, 71);

    // Test step panel: thresholds, progress, what went wrong.
    std::vector<Para> TestParas(int ex)
    {
        std::vector<Para> out;
        const Plan p = ComputePlan();
        const Fix& fix = g_fix[ex];
        if (!PlanReady(p, ex) || fix.restart)
        {
            out.push_back({ fix.restart ? fix.note : p.ex[ex].why, kErrInk });
            out.push_back({ T(tRestartAsk), kErrInk });
            return out;
        }
        out.push_back({ Fmt(T(kExercises[ex].threshold), p.ex[ex].down, p.ex[ex].top), kInfoInk });
        const TestRun& run = g_tests[ex];
        if (run.passed)
        {
            out.push_back({ Fmt(T(tTestPassed), run.counted, kTestReps), kOkInk });
            return out;
        }
        if (!fix.note.empty()) out.push_back({ fix.note, kWarnInk });
        out.push_back({ Fmt(T(tTestCounted), run.counted, kTestReps), kTextInk });
        if (run.last.seq != 0 && !run.last.counted)
            out.push_back({ WhyNotCounted(ex, run.last, p), kErrInk });
        if (g_testExercise.load() == ex && GetTickCount64() - run.postureOkMs > 2500)
            out.push_back({ T(kExercises[ex].noPose), kWarnInk });
        else if (run.notInPlank)
            out.push_back({ T(tTestStraight), kWarnInk });
        return out;
    }

    std::wstring TestSummary(int ex)
    {
        const TestRun& t = g_tests[ex];
        std::wstring s = t.passed ? Fmt(T(tSumTestOk), t.counted, kTestReps)
                       : t.skipped ? std::wstring(T(tSumTestSkipped))
                                   : Fmt(T(tSumTestWarn), t.counted, kTestReps);
        if (g_fix[ex].on) s += L"\n" + std::wstring(T(tSumAdjusted));
        return s;
    }

    // "Retake photos" on a check that cannot pass: back to the exercise's
    // first photo, everything about it forgotten.
    void GoToStep(int step);
    void RetakeExercise(int ex)
    {
        ResetExercise(ex);
        int first = -1;
        for (int i = 0; i < kSteps; ++i)
        {
            const StepDef& s = kStepDefs[i];
            if (s.kind != StepKind::Photo || s.exercise != ex) continue;
            g_shots[s.shot] = Shot{};
            if (first < 0) first = i;
        }
        LogF(L"calibrate: %s - retaking photos", ExerciseName(ex));
        if (first >= 0) GoToStep(first);
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
                s += e.why.empty() ? Fmt(T(tSumKept), g_uk ? kExercises[ex].nameUk : kExercises[ex].nameEn) : e.why;
                continue;
            }
            s += Fmt(T(kExercises[ex].summary),
                     OldVal(k.section, k.key[0]).c_str(), e.top, OldVal(k.section, k.key[1]).c_str(), e.goingDown,
                     OldVal(k.section, k.key[2]).c_str(), e.goingUp, OldVal(k.section, k.key[3]).c_str(), e.down);
            if (ex == kSquat && p.hip)
                s += L"\n" + Fmt(T(tSumHip), OldVal(L"squat", L"min_hip_drop").c_str(), p.hipDrop);
            s += L"\n" + TestSummary(ex);
        }
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

    ExerciseUnlock::Calib::SettingsRequest SettingsFor(HWND owner)
    {
        ExerciseUnlock::Calib::SettingsRequest req;
        req.inst = g_inst;
        req.owner = owner;
        req.uk = g_uk;
        req.configPath = g_configPath;
        req.soundsPath = ExerciseUnlock::DataPath(L"sounds.ini");
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

    // Only from steps without a running test: it reads config.ini again,
    // over the thresholds a test put into Config.
    void OpenSettings()
    {
        const auto r = ExerciseUnlock::Calib::ShowSettings(SettingsFor(g_wnd));
        if (!r.saved) return;
        Config::Load();
        g_cfgMinHipDrop = Config::SquatMinHipDrop();
        const int pick = PreferredCamera();
        if (pick >= 0 && pick != static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0)))
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
        const bool setCamera = g_cameras.size() > 1 && cam >= 0;
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
            for (int i = 0; i < 4; ++i) put(k.section, k.key[i], std::to_wstring(v[i]));
            if (ex == kSquat && p.hip) put(L"squat", L"min_hip_drop", Fmt(L"%.2f", p.hipDrop));
            LogF(L"calibrate: saved %s %d/%d/%d/%d", ExerciseName(ex), e.top, e.goingDown, e.goingUp, e.down);
        }
        // Only pin the camera when there is a choice to make: with a
        // single camera a pinned name would just make the service wait
        // if it ever gets renamed / replaced.
        if (setCamera) put(L"camera", L"preferred", g_cameras[static_cast<size_t>(cam)].name);
        // The angles are measured on x / width, y / height of this camera's
        // frame, so they depend on its aspect ratio. The phone camera page
        // rescales its poses to it (service PhoneLink sends it along).
        for (int i = 1; i < kShotCount; ++i)
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
        if (p.hip) LogF(L"calibrate: saved squat hip drop %.2f", p.hipDrop);
        g_status = T(tSaved);
        if (!g_setupMode) g_status += RestartService() ? T(tRestarted) : T(tRestartFailed);
        g_statusIsError = false;
        return true;
    }

    // ------------------------------------------------------------ capture
    void FinishCapture()
    {
        Shot& shot = g_shots[Cur().shot];
        g_phase = Phase::Live;
        if (g_samples.size() < kSamples / 2)
        {
            g_status = T(kExercises[Cur().exercise].notSeen);
            g_statusIsError = true;
            return;
        }
        ResetExercise(Cur().exercise);   // new photo, new thresholds: check again
        shot.done = true;
        shot.skipped = false;
        shot.angle = Median(g_samples);
        shot.hipY = Median(g_hipSamples);
        {
            std::lock_guard<std::mutex> lk(g_frameMx);
            shot.photo = g_latest;
        }
        LogF(L"calibrate: photo %d angle=%.1f hipY=%.3f samples=%zu", Cur().shot, shot.angle, shot.hipY, g_samples.size());
        g_status.clear();
    }

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
            const bool squat = Cur().exercise == kSquat;
            const float v = kExercises[Cur().exercise].measure(pose);
            if (v > 0.0f)
            {
                g_samples.push_back(v);
                const float hy = HipY(pose);
                if (squat && hy > 0.0f) g_hipSamples.push_back(hy);
            }
            if (g_samples.size() >= kSamples) EndCapture();
        }
        PollTest();
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    // ------------------------------------------------------------ UI
    void UpdateControls()
    {
        const StepDef& s = Cur();
        const bool cameraStep = s.kind == StepKind::Camera;
        const bool photoStep = s.kind == StepKind::Photo;
        const bool testStep = s.kind == StepKind::Test;
        const bool summary = s.kind == StepKind::Summary;
        const bool passed = testStep && g_tests[s.exercise].passed;
        // A check that cannot pass offers new photos instead of a new start.
        const bool retake = testStep && (g_fix[s.exercise].restart || !PlanReady(ComputePlan(), s.exercise));
        const bool busy = g_phase != Phase::Live;
        EnableWindow(g_back, g_step > 0 && !busy && !(summary && g_saved));
        ShowWindow(g_take, photoStep || testStep ? SW_SHOW : SW_HIDE);
        // With nothing to skip, the button opens the settings.
        SetWindowTextW(g_skip, T(cameraStep || summary ? tSettings : tSkip));
        EnableWindow(g_take, !busy && (retake || ((photoStep || testStep) && !g_cameras.empty())));
        EnableWindow(g_skip, !busy && !passed);
        EnableWindow(g_cam, !busy);
        if (photoStep) SetWindowTextW(g_take, T(g_shots[s.shot].done ? tRetake : tTake));
        if (testStep) SetWindowTextW(g_take, T(retake ? tRetakePhotos : tRestart));
        SetWindowTextW(g_next, T(summary ? (g_saved ? tClose : tSave) : tNext));
        EnableWindow(g_next, !busy && (photoStep ? g_shots[s.shot].done : !testStep || passed));
        InvalidateRect(g_wnd, nullptr, FALSE);
    }

    void GoToStep(int step)
    {
        const int dir = step < g_step ? -1 : 1;
        step = std::clamp(step, 0, kSteps - 1);
        // Skipped photos skip their test too (the camera and summary steps
        // at both ends are never tests, so this stays in range).
        while (kStepDefs[step].kind == StepKind::Test && !PhotosDone(kStepDefs[step].exercise))
            step += dir;
        StopTest();
        g_step = step;
        g_phase = Phase::Live;
        g_status.clear();
        if (Cur().kind == StepKind::Test && !g_tests[Cur().exercise].passed) StartTest(Cur().exercise);
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
        const StepDef& s = Cur();
        Frame f;
        const bool showPhoto = s.kind == StepKind::Photo && g_shots[s.shot].done && g_phase == Phase::Live;
        if (showPhoto) f = g_shots[s.shot].photo;
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
        if (s.kind == StepKind::Photo)
        {
            const ExerciseInfo& info = kExercises[s.exercise];
            const float a = info.measure(f.pose);
            std::wstring label = (a > 0) ? Fmt(T(info.angle), static_cast<int>(std::lround(a))) : std::wstring(L"?");
            if (showPhoto) label = Fmt(T(info.angle), static_cast<int>(std::lround(g_shots[s.shot].angle)));
            RECT lr{ v.left + Px(12), v.top + Px(8), v.right, v.top + Px(40) };
            SelectObject(dc, g_fontTitle);
            DrawShadowText(dc, label, lr, DT_LEFT | DT_SINGLELINE, RGB(255, 230, 90));
        }
        else if (s.kind == StepKind::Test && PlanReady(ComputePlan(), s.exercise) && !g_fix[s.exercise].restart)
        {
            // Rep counter, and the angle the detector sees right now.
            const TestRun& run = g_tests[s.exercise];
            std::wstring big = Fmt(L"%d / %d", run.counted, kTestReps);
            if (run.passed) big += L"  \x2714";
            RECT br{ v.left + Px(16), v.top + Px(4), v.right, v.top + Px(70) };
            SelectObject(dc, g_fontBig);
            DrawShadowText(dc, big, br, DT_LEFT | DT_SINGLELINE, run.passed ? RGB(90, 230, 140) : RGB(255, 230, 90));
            const bool seen = GetTickCount64() - run.postureOkMs < 1000;
            if (!run.passed && seen && run.angle > 0)
            {
                RECT lr{ v.left + Px(18), v.top + Px(72), v.right, v.top + Px(104) };
                SelectObject(dc, g_fontTitle);
                DrawShadowText(dc, Fmt(T(kExercises[s.exercise].angle), static_cast<int>(std::lround(run.angle))), lr,
                               DT_LEFT | DT_SINGLELINE, RGB(255, 255, 255));
            }
        }
    }

    // ------------------------------------------------------------ example pictures
    // A side-view figure per example (StepDef::example): 0 shows where the
    // camera goes, 1-4 what the camera should see for that photo. Figure
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
        // Soft highlight behind the joint whose angle this photo measures.
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

        // Checklist above the buttons: one row per photo and per test.
        // Not on the summary: it says the same and needs the room.
        const bool showList = step.kind != StepKind::Summary;
        const int rowH = Px(20);
        int rows = 0;
        for (const StepDef& s : kStepDefs)
            if (showList && (s.kind == StepKind::Photo || s.kind == StepKind::Test)) ++rows;
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
        if (step.kind == StepKind::Test)
            for (const Para& t : TestParas(step.exercise)) para(t.text, t.color);

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
        for (const StepDef& s : kStepDefs)
        {
            if (!showList) break;
            bool done = false;
            std::wstring label, v;
            if (s.kind == StepKind::Photo)
            {
                const Shot& shot = g_shots[s.shot];
                done = shot.done;
                int topShot = 0, downShot = 0;
                PhotoShots(s.exercise, topShot, downShot);
                const ExerciseInfo& info = kExercises[s.exercise];
                label = T(s.shot == topShot ? info.rowTop : info.rowDown);
                v = shot.done ? Fmt(T(info.angle), static_cast<int>(std::lround(shot.angle)))
                              : (shot.skipped ? T(tSkipped) : T(tNotYet));
            }
            else if (s.kind == StepKind::Test)
            {
                const TestRun& t = g_tests[s.exercise];
                done = t.passed;
                label = T(s.title);
                v = t.passed || t.counted > 0 ? Fmt(L"%d/%d", t.counted, kTestReps)
                                              : (t.skipped ? T(tSkipped) : T(tNotYet));
            }
            else
                continue;
            const std::wstring line = std::wstring(done ? L"\x2714  " : L"\x2022  ") + label + L": " + v;
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

    void OnCommand(int id, int code)
    {
        switch (id)
        {
        case idCamera:
            if (code == CBN_SELCHANGE)
            {
                g_status.clear();
                StartCamera(static_cast<int>(SendMessageW(g_cam, CB_GETCURSEL, 0, 0)));
                // Another camera sees the child differently: count from zero.
                if (Cur().kind == StepKind::Test && !g_tests[Cur().exercise].passed) StartTest(Cur().exercise);
                InvalidateRect(g_wnd, nullptr, FALSE);
            }
            break;
        case idBack:
            GoToStep(g_step - 1);
            break;
        case idTake:
            g_status.clear();
            if (Cur().kind == StepKind::Test)
            {
                const int ex = Cur().exercise;
                if (g_fix[ex].restart || !PlanReady(ComputePlan(), ex))
                    RetakeExercise(ex);      // "Retake photos"
                else
                {
                    StartTest(ex);           // "Start over"
                    UpdateControls();
                }
                break;
            }
            g_samples.clear();
            g_hipSamples.clear();
            g_shots[Cur().shot].done = false;
            ResetExercise(Cur().exercise);
            g_phase = Phase::Collecting;   // the next ~12 frames give the angle (median)
            g_phaseStart = GetTickCount64();
            UpdateControls();
            break;
        case idSkip:
            if (Cur().kind == StepKind::Camera || Cur().kind == StepKind::Summary)
            {
                OpenSettings();
                break;
            }
            if (Cur().kind == StepKind::Test)
                g_tests[Cur().exercise].skipped = true;
            else
            {
                g_shots[Cur().shot].done = false;
                g_shots[Cur().shot].skipped = true;
                ResetExercise(Cur().exercise);
            }
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
            for (const CameraInfo& c : g_cameras)
                SendMessageW(g_cam, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.name.c_str()));
            if (!g_cameras.empty())
            {
                const int pick = std::max(PreferredCamera(), 0);
                SendMessageW(g_cam, CB_SETCURSEL, static_cast<WPARAM>(pick), 0);
                if (g_snapshotPath.empty()) StartCamera(pick);
            }
            if (!g_poseOk) { g_status = T(tNoModel); g_statusIsError = true; }
            Layout();
            if (!g_snapshotPath.empty())
            {
                // Layout check without a camera: sample measurements for the
                // photos before this step, the squat check passed and a
                // push-up check under way (3 of 5, last rep too shallow).
                g_snapshotStep = std::clamp(g_snapshotStep, 0, kSteps - 1);
                const float sample[kShotCount] = { 0.0f, 171.0f, 96.0f, 164.0f, 108.0f, 118.0f, 40.0f };
                for (int i = 0; i < kSteps; ++i)
                {
                    const StepDef& s = kStepDefs[i];
                    if (s.kind != StepKind::Photo || i >= g_snapshotStep) continue;
                    g_shots[s.shot].done = true;
                    g_shots[s.shot].angle = sample[s.shot];
                    g_shots[s.shot].hipY = (s.shot == 1) ? 0.50f : 0.62f;
                }
                GoToStep(g_snapshotStep);
                RepResult shallow;
                shallow.seq = 1;
                shallow.reachedDown = false;
                shallow.deepest = 142.0f;
                const int current = Cur().kind == StepKind::Test ? Cur().exercise : kPushUp;
                for (int ex = 0; ex < kExerciseCount; ++ex)
                {
                    if (!PhotosDone(ex)) continue;
                    TestRun& t = g_tests[ex];
                    t.counted = ex == current ? 3 : kTestReps;
                    t.passed = ex != current;
                    t.last = ex == current ? shallow : RepResult{};
                    t.postureOkMs = GetTickCount64();   // no "not recognized" hint in the shot
                }
                // And the two check verdicts: squats and sit-ups adjusted,
                // push-ups need new photos.
                if (Cur().kind == StepKind::Test)
                {
                    Fix& fx = g_fix[current];
                    if (current != kPushUp)
                    {
                        const ExercisePlan e = ComputePlan().ex[current];
                        fx.on = true;
                        fx.st = e.top - 2; fx.gd = e.goingDown - 2; fx.gu = e.goingUp + 6; fx.dn = e.down + 8;
                        fx.rounds = 1;
                        fx.note = Fmt(T(tTestAdjusted), (Fmt(T(kExercises[current].adjDown), e.down, fx.dn) + L", " +
                                                         Fmt(T(kExercises[current].adjTop), e.top, fx.st)).c_str());
                    }
                    else
                    {
                        fx.restart = true;
                        fx.note = Fmt(T(tRestartRange), 7);
                    }
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
    bool settingsOnly = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"/setup") == 0) g_setupMode = true;
            else if (_wcsicmp(argv[i], L"/settings") == 0) settingsOnly = true;
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
    // The test steps run the service's detectors: the same config.ini (the
    // service reads the one next to its exe, which is this folder), any
    // time of day.
    Config::Load();
    Config::SetIgnoreSchedule(true);
    g_cfgMinHipDrop = Config::SquatMinHipDrop();

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
    LogF(L"calibrate: start lang=%s setup=%d pose=%d", g_uk ? L"uk" : L"en", g_setupMode ? 1 : 0, g_poseOk ? 1 : 0);

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
