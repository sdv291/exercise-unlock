#define NOMINMAX
#include "Settings.h"
#include "Snapshot.h"
#include "Config.h"
#include "Log.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>

using ExerciseUnlock::Svc::Config;
using ExerciseUnlock::Svc::LogF;

namespace ExerciseUnlock::Calib
{
namespace
{
    enum Page { kTime, kSchedule, kSquats, kPushUps, kSitUps, kCamera, kPhone, kTexts, kSounds, kPageCount };

    struct PageText { const wchar_t* en; const wchar_t* uk; const wchar_t* noteEn; const wchar_t* noteUk; };

    constexpr wchar_t kAnglesEn[] = L"The calibration sets the angles and the hip drop itself and overwrites them the next time it runs. Change them here only to fine-tune.";
    constexpr wchar_t kAnglesUk[] = L"Калібрування задає кути й опускання стегон саме та перезаписує їх під час наступного запуску. Змінюйте їх тут лише для точного налаштування.";

    const PageText kPages[kPageCount] = {
        { L"Time", L"Час",
          L"How much computer time the exercises earn.",
          L"Скільки часу за комп'ютером дають вправи." },
        { L"Schedule", L"Розклад",
          L"Outside these hours reps are not credited and the computer does not unlock. From = to means no limit.",
          L"Поза цими годинами повтори не зараховуються, а комп'ютер не розблоковується. Якщо «від» = «до», обмеження немає." },
        { L"Squats", L"Присідання", kAnglesEn, kAnglesUk },
        { L"Push-ups", L"Віджимання", kAnglesEn, kAnglesUk },
        { L"Sit-ups", L"Прес", kAnglesEn, kAnglesUk },
        { L"Camera", L"Камера", nullptr, nullptr },
        { L"Phone and updates", L"Телефон і оновлення", nullptr, nullptr },
        { L"Texts", L"Тексти",
          L"The words on the lock screen tile.",
          L"Написи на плитці екрана блокування." },
        { L"Sounds", L"Звуки",
          L"WAV files (PCM). Several files separated by commas: one is picked at random. Empty: the standard Windows beep. Applied at once.",
          L"Файли WAV (PCM). Кілька файлів через кому: один вибирається випадково. Порожньо: стандартний сигнал Windows. Діє одразу." },
    };

    enum class Kind { Int, Float, Time, Text, Bool, Mode, Camera };

    template <int (*Get)()> std::wstring IntOf() { return std::to_wstring(Get()); }
    template <bool (*Get)()> std::wstring BoolOf() { return Get() ? L"1" : L"0"; }
    template <std::wstring (*Get)()> std::wstring TextOf() { return Get(); }
    template <float (*Get)()> std::wstring FloatOf()
    {
        wchar_t buf[32]{};
        swprintf_s(buf, L"%g", Get());
        return buf;
    }
    template <int Minutes> std::wstring TimeOf()
    {
        wchar_t buf[8]{};
        swprintf_s(buf, L"%02d:%02d", Minutes / 60, Minutes % 60);
        return buf;
    }
    std::wstring ModeDefault() { return L"angle"; }

    // One value in the file. `lo` / `hi`: its range, for Int and Float.
    // A required value can't be left empty. `fallback`: what the service
    // uses when the file doesn't have the key (none: it stays empty).
    struct Field
    {
        Page page;
        Kind kind;
        const wchar_t* section;
        const wchar_t* key;
        double lo, hi;
        bool required;
        std::wstring (*fallback)();
        const wchar_t* en;
        const wchar_t* uk;
        const wchar_t* hintEn;
        const wchar_t* hintUk;
    };

    constexpr double kMaxSeconds = 86400;   // a day
    constexpr double kRepMsLo = 100, kRepMsHi = 60000;

    const Field kFields[] = {
        // ------------------------------------------------------------ time
        { kTime, Kind::Int, L"reward", L"squat_seconds", 1, 3600, true, IntOf<Config::SquatSeconds>,
          L"Seconds for one squat", L"Секунд за один присід",
          L"Computer time one counted squat earns.",
          L"Час за комп'ютером, який дає один зарахований присід." },
        { kTime, Kind::Int, L"reward", L"pushup_seconds", 1, 3600, true, IntOf<Config::PushupSeconds>,
          L"Seconds for one push-up", L"Секунд за одне віджимання",
          L"Computer time one counted push-up earns.",
          L"Час за комп'ютером, який дає одне зараховане віджимання." },
        { kTime, Kind::Int, L"reward", L"situp_seconds", 1, 3600, true, IntOf<Config::SitupSeconds>,
          L"Seconds for one sit-up", L"Секунд за один підйом корпуса",
          L"Computer time one counted sit-up earns.",
          L"Час за комп'ютером, який дає один зарахований підйом корпуса." },
        { kTime, Kind::Int, L"reward", L"min_unlock_seconds", 1, kMaxSeconds, true, IntOf<Config::MinUnlockSeconds>,
          L"Minimum to unlock, seconds", L"Мінімум для розблокування, секунд",
          L"The computer unlocks only when at least this much is earned. 600 = 10 minutes.",
          L"Комп'ютер розблоковується, лише коли зароблено щонайменше стільки. 600 = 10 хвилин." },
        { kTime, Kind::Int, L"reward", L"max_session_seconds", 0, kMaxSeconds, true, IntOf<Config::MaxSessionSeconds>,
          L"Maximum to earn, seconds", L"Максимум заробітку, секунд",
          L"Reps above this limit add nothing (the error sound plays). 3600 = 1 hour, 0 = no limit.",
          L"Повтори понад цю межу нічого не додають (лунає звук помилки). 3600 = 1 година, 0 = без обмеження." },
        { kTime, Kind::Int, L"reward", L"grace_window_seconds", 0, kMaxSeconds, true, IntOf<Config::GraceWindowSeconds>,
          L"Grace period after a session, seconds", L"Пільговий період після сеансу, секунд",
          L"For this long after a session ends the child can get one short unlock to close their apps, without exercising. 0 = off.",
          L"Стільки часу після завершення сеансу дитина може один раз коротко розблокувати комп'ютер без вправ, щоб закрити програми. 0 = вимкнено." },
        { kTime, Kind::Int, L"reward", L"grace_unlock_seconds", 1, 3600, true, IntOf<Config::GraceUnlockSeconds>,
          L"Length of that unlock, seconds", L"Тривалість цього розблокування, секунд",
          L"How long the short unlock lasts.",
          L"Скільки триває коротке розблокування." },
        { kTime, Kind::Int, L"reward", L"progression_after_days", 0, 3650, true, IntOf<Config::ProgressionAfterDays>,
          L"Reps get cheaper after, days", L"Повтори дешевшають через, днів",
          L"Days after installation before reps start to earn less. 0 = reps never get cheaper.",
          L"Скільки днів після встановлення повтори дають повну ціну. 0 = не дешевшають ніколи." },
        { kTime, Kind::Float, L"reward", L"progression_factor", 0, 10, true, FloatOf<Config::ProgressionFactor>,
          L"How fast they get cheaper", L"Як швидко дешевшають",
          L"A rep is worth 1 / (1 + weeks x this number) of its price, but not less than a quarter. 0 = off.",
          L"Повтор коштує 1 / (1 + тижні x це число) своєї ціни, але не менше чверті. 0 = вимкнено." },

        // ------------------------------------------------------------ schedule
        { kSchedule, Kind::Time, L"schedule", L"active_from", 0, 0, true, TimeOf<7 * 60>,
          L"Allowed from", L"Дозволено від",
          L"Start of the allowed hours, HH:MM. Used for the days without their own hours below.",
          L"Початок дозволених годин, ГГ:ХХ. Діє для днів, які не мають власних годин нижче." },
        { kSchedule, Kind::Time, L"schedule", L"active_to", 0, 0, true, TimeOf<20 * 60>,
          L"Allowed to", L"Дозволено до",
          L"End of the allowed hours, HH:MM. If it is earlier than the start, the hours run overnight.",
          L"Кінець дозволених годин, ГГ:ХХ. Якщо він раніший за початок, години переходять через ніч." },
        { kSchedule, Kind::Time, L"schedule", L"weekday_from", 0, 0, false, nullptr,
          L"Monday-Friday: from", L"Понеділок-п'ятниця: від",
          L"Empty: the common hours above.",
          L"Порожньо: спільні години вище." },
        { kSchedule, Kind::Time, L"schedule", L"weekday_to", 0, 0, false, nullptr,
          L"Monday-Friday: to", L"Понеділок-п'ятниця: до",
          L"Empty: the common hours above.",
          L"Порожньо: спільні години вище." },
        { kSchedule, Kind::Time, L"schedule", L"weekend_from", 0, 0, false, nullptr,
          L"Saturday-Sunday: from", L"Субота-неділя: від",
          L"Empty: the common hours above.",
          L"Порожньо: спільні години вище." },
        { kSchedule, Kind::Time, L"schedule", L"weekend_to", 0, 0, false, nullptr,
          L"Saturday-Sunday: to", L"Субота-неділя: до",
          L"Empty: the common hours above.",
          L"Порожньо: спільні години вище." },
        { kSchedule, Kind::Int, L"schedule", L"weekday_min_unlock_seconds", 1, kMaxSeconds, false, nullptr,
          L"Monday-Friday: minimum to unlock, s", L"Понеділок-п'ятниця: мінімум, с",
          L"Empty: the minimum from the Time page.",
          L"Порожньо: мінімум зі сторінки «Час»." },
        { kSchedule, Kind::Int, L"schedule", L"weekend_min_unlock_seconds", 1, kMaxSeconds, false, nullptr,
          L"Saturday-Sunday: minimum to unlock, s", L"Субота-неділя: мінімум, с",
          L"Empty: the minimum from the Time page.",
          L"Порожньо: мінімум зі сторінки «Час»." },

        // ------------------------------------------------------------ squats
        { kSquats, Kind::Float, L"squat", L"standing_angle", 0, 180, true, FloatOf<Config::SquatStandingAngle>,
          L"Standing, knee angle", L"Стійка, кут коліна",
          L"The knee has to straighten to this angle for the rep to finish.",
          L"Коліно має випрямитися до цього кута, щоб повтор завершився." },
        { kSquats, Kind::Float, L"squat", L"going_down_angle", 0, 180, true, FloatOf<Config::SquatGoingDownAngle>,
          L"Going down, knee angle", L"Спуск, кут коліна",
          L"The rep starts when the knee bends below this angle.",
          L"Повтор починається, коли коліно згинається нижче цього кута." },
        { kSquats, Kind::Float, L"squat", L"going_up_angle", 0, 180, true, FloatOf<Config::SquatGoingUpAngle>,
          L"Going up, knee angle", L"Підйом, кут коліна",
          L"The way up starts when the knee opens above this angle.",
          L"Підйом починається, коли коліно розгинається вище цього кута." },
        { kSquats, Kind::Float, L"squat", L"down_angle", 0, 180, true, FloatOf<Config::SquatDownAngle>,
          L"Bottom, knee angle", L"Низ, кут коліна",
          L"The knee has to bend to this angle or less for the rep to count.",
          L"Коліно має зігнутися до цього кута або менше, щоб повтор зарахувався." },
        { kSquats, Kind::Int, L"squat", L"min_rep_ms", kRepMsLo, kRepMsHi, true, IntOf<Config::SquatMinRepMs>,
          L"Shortest rep, ms", L"Найкоротший повтор, мс",
          L"A faster rep does not count. 1000 ms = 1 second.",
          L"Швидший повтор не зараховується. 1000 мс = 1 секунда." },
        { kSquats, Kind::Int, L"squat", L"max_rep_ms", kRepMsLo, kRepMsHi, true, IntOf<Config::SquatMaxRepMs>,
          L"Longest rep, ms", L"Найдовший повтор, мс",
          L"A slower rep does not count.",
          L"Повільніший повтор не зараховується." },
        { kSquats, Kind::Float, L"squat", L"min_hip_drop", 0, 1, true, FloatOf<Config::SquatMinHipDrop>,
          L"Minimum hip drop", L"Мінімальне опускання стегон",
          L"In parts of the frame height. Tells a squat from lifting a knee. 0 = off.",
          L"У частках висоти кадру. Відрізняє присідання від піднімання коліна. 0 = вимкнено." },
        { kSquats, Kind::Float, L"squat", L"max_hip_drift", 0, 1, true, FloatOf<Config::SquatMaxHipDrift>,
          L"Maximum sideways hip shift", L"Максимальний зсув стегон убік",
          L"In parts of the frame width. Tells a squat from walking across the frame. 0 = off.",
          L"У частках ширини кадру. Відрізняє присідання від ходьби в кадрі. 0 = вимкнено." },
        { kSquats, Kind::Float, L"squat", L"quality_min", 0, 1, true, FloatOf<Config::SquatQualityMin>,
          L"Minimum depth of a rep", L"Мінімальна глибина повтору",
          L"The part of the way from standing to the bottom a rep has to cover, 0..1. 0 = off.",
          L"Яку частину шляху від стійки до низу має пройти повтор, 0..1. 0 = вимкнено." },
        { kSquats, Kind::Mode, L"squat", L"mode", 0, 0, true, ModeDefault,
          L"What is measured", L"Що вимірюється",
          L"Knee angle is the reliable choice. Hip height uses the four values below.",
          L"Кут коліна - надійний вибір. Висота стегон використовує чотири значення нижче." },
        { kSquats, Kind::Float, L"squat", L"standing_hip_frac", 0, 1, true, FloatOf<Config::SquatStandingHipFrac>,
          L"Hip height: standing", L"Висота стегон: стійка",
          L"Only for the hip height mode: hip height between ankle (0) and shoulder (1).",
          L"Лише для режиму висоти стегон: висота стегон між щиколоткою (0) і плечем (1)." },
        { kSquats, Kind::Float, L"squat", L"going_down_hip_frac", 0, 1, true, FloatOf<Config::SquatGoingDownHipFrac>,
          L"Hip height: going down", L"Висота стегон: спуск",
          L"Only for the hip height mode.",
          L"Лише для режиму висоти стегон." },
        { kSquats, Kind::Float, L"squat", L"going_up_hip_frac", 0, 1, true, FloatOf<Config::SquatGoingUpHipFrac>,
          L"Hip height: going up", L"Висота стегон: підйом",
          L"Only for the hip height mode.",
          L"Лише для режиму висоти стегон." },
        { kSquats, Kind::Float, L"squat", L"down_hip_frac", 0, 1, true, FloatOf<Config::SquatDownHipFrac>,
          L"Hip height: bottom", L"Висота стегон: низ",
          L"Only for the hip height mode.",
          L"Лише для режиму висоти стегон." },

        // ------------------------------------------------------------ push-ups
        { kPushUps, Kind::Float, L"pushup", L"up_angle", 0, 180, true, FloatOf<Config::PushupUpAngle>,
          L"Arms straight, elbow angle", L"Прямі руки, кут ліктя",
          L"The elbow has to straighten to this angle for the rep to finish.",
          L"Лікоть має випрямитися до цього кута, щоб повтор завершився." },
        { kPushUps, Kind::Float, L"pushup", L"going_down_angle", 0, 180, true, FloatOf<Config::PushupGoingDownAngle>,
          L"Going down, elbow angle", L"Спуск, кут ліктя",
          L"The rep starts when the elbow bends below this angle.",
          L"Повтор починається, коли лікоть згинається нижче цього кута." },
        { kPushUps, Kind::Float, L"pushup", L"going_up_angle", 0, 180, true, FloatOf<Config::PushupGoingUpAngle>,
          L"Going up, elbow angle", L"Підйом, кут ліктя",
          L"The way up starts when the elbow opens above this angle.",
          L"Підйом починається, коли лікоть розгинається вище цього кута." },
        { kPushUps, Kind::Float, L"pushup", L"down_angle", 0, 180, true, FloatOf<Config::PushupDownAngle>,
          L"Bottom, elbow angle", L"Низ, кут ліктя",
          L"The elbow has to bend to this angle or less for the rep to count.",
          L"Лікоть має зігнутися до цього кута або менше, щоб повтор зарахувався." },
        { kPushUps, Kind::Float, L"pushup", L"plank_min_angle", 0, 180, true, FloatOf<Config::PushupPlankMinAngle>,
          L"Plank, minimum body angle", L"Планка, мінімальний кут тіла",
          L"Angle shoulder-hip-ankle. Kept low on purpose: children often raise their hips.",
          L"Кут плече-стегно-щиколотка. Навмисно низький: діти часто піднімають стегна." },
        { kPushUps, Kind::Int, L"pushup", L"min_rep_ms", kRepMsLo, kRepMsHi, true, IntOf<Config::PushupMinRepMs>,
          L"Shortest rep, ms", L"Найкоротший повтор, мс",
          L"A faster rep does not count. 1000 ms = 1 second.",
          L"Швидший повтор не зараховується. 1000 мс = 1 секунда." },
        { kPushUps, Kind::Int, L"pushup", L"max_rep_ms", kRepMsLo, kRepMsHi, true, IntOf<Config::PushupMaxRepMs>,
          L"Longest rep, ms", L"Найдовший повтор, мс",
          L"A slower rep does not count.",
          L"Повільніший повтор не зараховується." },
        { kPushUps, Kind::Float, L"pushup", L"quality_min", 0, 1, true, FloatOf<Config::PushupQualityMin>,
          L"Minimum depth of a rep", L"Мінімальна глибина повтору",
          L"The part of the way from straight arms to the bottom a rep has to cover, 0..1. 0 = off.",
          L"Яку частину шляху від прямих рук до низу має пройти повтор, 0..1. 0 = вимкнено." },

        // ------------------------------------------------------------ sit-ups
        { kSitUps, Kind::Float, L"situp", L"lying_angle", 0, 180, true, FloatOf<Config::SitupLyingAngle>,
          L"Lying, hip angle", L"Лежачи, кут у стегні",
          L"The hip angle has to open to this value for the rep to finish.",
          L"Кут у стегні має розкритися до цього значення, щоб повтор завершився." },
        { kSitUps, Kind::Float, L"situp", L"rising_angle", 0, 180, true, FloatOf<Config::SitupRisingAngle>,
          L"Rising, hip angle", L"Підйом, кут у стегні",
          L"The rep starts when the angle closes below this value.",
          L"Повтор починається, коли кут зменшується нижче цього значення." },
        { kSitUps, Kind::Float, L"situp", L"lowering_angle", 0, 180, true, FloatOf<Config::SitupLoweringAngle>,
          L"Lowering, hip angle", L"Опускання, кут у стегні",
          L"The way down starts when the angle opens above this value.",
          L"Опускання починається, коли кут збільшується вище цього значення." },
        { kSitUps, Kind::Float, L"situp", L"sitting_angle", 0, 180, true, FloatOf<Config::SitupSittingAngle>,
          L"Top, hip angle", L"Угорі, кут у стегні",
          L"The hip angle has to close to this value or less for the rep to count.",
          L"Кут у стегні має зменшитися до цього значення або менше, щоб повтор зарахувався." },
        { kSitUps, Kind::Int, L"situp", L"min_rep_ms", kRepMsLo, kRepMsHi, true, IntOf<Config::SitupMinRepMs>,
          L"Shortest rep, ms", L"Найкоротший повтор, мс",
          L"A faster rep does not count. 1000 ms = 1 second.",
          L"Швидший повтор не зараховується. 1000 мс = 1 секунда." },
        { kSitUps, Kind::Int, L"situp", L"max_rep_ms", kRepMsLo, kRepMsHi, true, IntOf<Config::SitupMaxRepMs>,
          L"Longest rep, ms", L"Найдовший повтор, мс",
          L"A slower rep does not count.",
          L"Повільніший повтор не зараховується." },
        { kSitUps, Kind::Float, L"situp", L"quality_min", 0, 1, true, FloatOf<Config::SitupQualityMin>,
          L"Minimum height of a rep", L"Мінімальна висота повтору",
          L"The part of the way from lying to the top a rep has to cover, 0..1. 0 = off.",
          L"Яку частину шляху від положення лежачи до верху має пройти повтор, 0..1. 0 = вимкнено." },

        // ------------------------------------------------------------ camera
        { kCamera, Kind::Camera, L"camera", L"preferred", 0, 0, false, TextOf<Config::CameraPreference>,
          L"Camera", L"Камера",
          L"Part of the camera's name is enough. Empty: the first camera found.",
          L"Достатньо частини назви камери. Порожньо: перша знайдена камера." },
        { kCamera, Kind::Bool, L"camera", L"lock_screen_only", 0, 0, true, BoolOf<Config::CameraLockScreenOnly>,
          L"Camera only on the lock screen", L"Камера лише на екрані блокування",
          L"Off: the service keeps the camera on whenever no session is running, also on the desktop.",
          L"Вимкнено: служба тримає камеру ввімкненою весь час поза сеансом, зокрема на робочому столі." },
        { kCamera, Kind::Float, L"calibration", L"aspect", 0.5, 4, false, nullptr,
          L"Picture width / height", L"Ширина / висота кадру",
          L"Written by the calibration: the phone rescales its poses to it. Empty: taken from the webcam.",
          L"Записує калібрування: телефон перераховує свої пози під це значення. Порожньо: береться з вебкамери." },
        { kCamera, Kind::Int, L"security", L"liveness_max_static_frames", 0, 100000, true, IntOf<Config::LivenessMaxStaticFrames>,
          L"Frozen picture limit, frames", L"Межа застиглого кадру, кадрів",
          L"This many identical frames in a row mean a fake feed, and the camera is refused. 30 = about 1 s. 0 = off.",
          L"Стільки однакових кадрів поспіль вважаються підробкою, і камера відхиляється. 30 = приблизно 1 с. 0 = вимкнено." },
        { kCamera, Kind::Text, L"security", L"virtual_camera_blocklist", 0, 0, false, TextOf<Config::VirtualCameraBlocklist>,
          L"Refused cameras", L"Заборонені камери",
          L"Parts of camera names, separated by commas: virtual cameras that could play back a recording.",
          L"Частини назв камер через кому: віртуальні камери, які могли б відтворювати запис." },

        // ------------------------------------------------------------ phone, updates
        { kPhone, Kind::Bool, L"phone", L"enabled", 0, 0, true, BoolOf<Config::PhoneEnabled>,
          L"Phone camera", L"Камера телефона",
          L"The lock screen shows a QR code: the child scans it and exercises in front of the phone.",
          L"Екран блокування показує QR-код: дитина сканує його й виконує вправи перед телефоном." },
        { kPhone, Kind::Text, L"phone", L"relay", 0, 0, false, TextOf<Config::PhoneRelayUrl>,
          L"Relay server", L"Сервер-посередник",
          L"Connects the PC and the phone. Has to be https:// - the phone's camera needs it. Empty: no phone camera.",
          L"З'єднує комп'ютер і телефон. Має бути https:// - цього потребує камера телефона. Порожньо: без камери телефона." },
        { kPhone, Kind::Bool, L"update", L"enabled", 0, 0, true, BoolOf<Config::UpdatesEnabled>,
          L"Check for updates", L"Перевіряти оновлення",
          L"When a new version is out, the lock screen says so and the desktop shows a notification.",
          L"Коли виходить нова версія, про це повідомляє екран блокування й сповіщення на робочому столі." },
        { kPhone, Kind::Text, L"update", L"manifest_url", 0, 0, false, TextOf<Config::UpdateManifestUrl>,
          L"Version file address", L"Адреса файла версії",
          L"https:// address of the small file with the latest version. Empty: no check.",
          L"Адреса https:// маленького файла з останньою версією. Порожньо: без перевірки." },
        { kPhone, Kind::Int, L"update", L"check_interval_hours", 0, 167, true, IntOf<Config::UpdateIntervalHours>,
          L"Check every, hours", L"Перевіряти кожні, годин",
          L"0: only when the service starts.",
          L"0: лише під час запуску служби." },

        // ------------------------------------------------------------ texts
        { kTexts, Kind::Text, L"labels", L"squats", 0, 0, true, TextOf<Config::LabelSquats>,
          L"Squats", L"Присідання",
          L"Name of the squat counter.",
          L"Назва лічильника присідань." },
        { kTexts, Kind::Text, L"labels", L"pushups", 0, 0, true, TextOf<Config::LabelPushups>,
          L"Push-ups", L"Віджимання",
          L"Name of the push-up counter.",
          L"Назва лічильника віджимань." },
        { kTexts, Kind::Text, L"labels", L"situps", 0, 0, true, TextOf<Config::LabelSitups>,
          L"Sit-ups", L"Прес",
          L"Name of the sit-up counter.",
          L"Назва лічильника підйомів корпуса." },
        { kTexts, Kind::Text, L"labels", L"earned", 0, 0, true, TextOf<Config::LabelEarned>,
          L"Earned time", L"Зароблений час",
          L"Goes before the earned time.",
          L"Стоїть перед заробленим часом." },
        { kTexts, Kind::Text, L"labels", L"ready", 0, 0, true, TextOf<Config::LabelReady>,
          L"Ready to unlock", L"Можна розблокувати",
          L"Added after the earned time once it reaches the minimum.",
          L"Додається після заробленого часу, коли він досягає мінімуму." },
        { kTexts, Kind::Text, L"labels", L"outside_hours", 0, 0, true, TextOf<Config::LabelOutsideHours>,
          L"Outside allowed hours", L"Поза дозволеними годинами",
          L"Shown outside the schedule. {from} and {to} become the allowed hours.",
          L"Показується поза розкладом. {from} і {to} замінюються на дозволені години." },
        { kTexts, Kind::Text, L"warnings", L"lock_in_message", 0, 0, true, TextOf<Config::LockInMessage>,
          L"Session is about to end", L"Сеанс скоро завершиться",
          L"Notification before the computer locks. {min} becomes the minutes left.",
          L"Сповіщення перед блокуванням. {min} замінюється на кількість хвилин, що залишилися." },

        // ------------------------------------------------------------ sounds
        { kSounds, Kind::Text, L"sounds", L"squat_ready", 0, 0, false, nullptr,
          L"Squat position recognized", L"Положення для присідань розпізнано",
          L"Plays when the child stands ready to squat.",
          L"Лунає, коли дитина стала в положення для присідань." },
        { kSounds, Kind::Text, L"sounds", L"pushup_ready", 0, 0, false, nullptr,
          L"Push-up position recognized", L"Положення для віджимань розпізнано",
          L"Plays when the child takes the plank position.",
          L"Лунає, коли дитина стала в планку." },
        { kSounds, Kind::Text, L"sounds", L"situp_ready", 0, 0, false, nullptr,
          L"Sit-up position recognized", L"Положення для преса розпізнано",
          L"Plays when the child lies down for sit-ups. Empty: the push-up sound.",
          L"Лунає, коли дитина лягла для преса. Порожньо: звук віджимань." },
        { kSounds, Kind::Text, L"sounds", L"squat_counted", 0, 0, false, nullptr,
          L"Squat counted", L"Присідання зараховано",
          L"Plays for each counted squat.",
          L"Лунає за кожне зараховане присідання." },
        { kSounds, Kind::Text, L"sounds", L"pushup_counted", 0, 0, false, nullptr,
          L"Push-up counted", L"Віджимання зараховано",
          L"Plays for each counted push-up.",
          L"Лунає за кожне зараховане віджимання." },
        { kSounds, Kind::Text, L"sounds", L"situp_counted", 0, 0, false, nullptr,
          L"Sit-up counted", L"Підйом корпуса зараховано",
          L"Plays for each counted sit-up. Empty: the push-up sound.",
          L"Лунає за кожен зарахований підйом корпуса. Порожньо: звук віджимань." },
        { kSounds, Kind::Text, L"sounds", L"error", 0, 0, false, nullptr,
          L"Error", L"Помилка",
          L"Plays for a rep that earns nothing: outside the allowed hours, above the maximum, or not counted.",
          L"Лунає за повтор, який нічого не дає: поза дозволеними годинами, понад максимум або незарахований." },
        { kSounds, Kind::Text, L"sounds", L"library", 0, 0, false, nullptr,
          L"Sounds folder", L"Папка зі звуками",
          L"Where to look for files given by name. Empty: the sounds folder next to sounds.ini.",
          L"Де шукати файли, указані лише за назвою. Порожньо: папка sounds поруч із sounds.ini." },
    };
    constexpr int kFieldCount = static_cast<int>(std::size(kFields));

    // Values that have to fall in the order listed: a rep's thresholds from
    // the rest pose to the other end, its longest time to its shortest.
    struct Rule
    {
        const wchar_t* section;
        const wchar_t* keys[4];
        const wchar_t* whatEn;
        const wchar_t* whatUk;
    };
    constexpr wchar_t kFallsEn[] = L"The four angles of \"%s\" must go down from the first to the last.";
    constexpr wchar_t kFallsUk[] = L"Чотири кути вправи «%s» мають зменшуватися від першого до останнього.";
    constexpr wchar_t kRisesEn[] = L"\"%s\": the longest rep must be longer than the shortest.";
    constexpr wchar_t kRisesUk[] = L"«%s»: найдовший повтор має бути довшим за найкоротший.";
    const Rule kRules[] = {
        { L"squat",  { L"standing_angle", L"going_down_angle", L"going_up_angle", L"down_angle" }, kFallsEn, kFallsUk },
        { L"pushup", { L"up_angle",       L"going_down_angle", L"going_up_angle", L"down_angle" }, kFallsEn, kFallsUk },
        { L"situp",  { L"lying_angle",    L"rising_angle",     L"lowering_angle", L"sitting_angle" }, kFallsEn, kFallsUk },
        { L"squat",  { L"max_rep_ms", L"min_rep_ms" }, kRisesEn, kRisesUk },
        { L"pushup", { L"max_rep_ms", L"min_rep_ms" }, kRisesEn, kRisesUk },
        { L"situp",  { L"max_rep_ms", L"min_rep_ms" }, kRisesEn, kRisesUk },
    };

    // ------------------------------------------------------------ values
    std::wstring Trim(std::wstring s)
    {
        const wchar_t* blank = L" \t\r\n";
        const size_t a = s.find_first_not_of(blank);
        if (a == std::wstring::npos) return {};
        return s.substr(a, s.find_last_not_of(blank) - a + 1);
    }

    bool ParseNumber(std::wstring s, double& out)
    {
        std::replace(s.begin(), s.end(), L',', L'.');
        wchar_t* end = nullptr;
        out = wcstod(s.c_str(), &end);
        return end != s.c_str() && *end == L'\0' && std::isfinite(out);
    }

    bool ParseTime(const std::wstring& s, int& h, int& m)
    {
        wchar_t* end = nullptr;
        h = static_cast<int>(wcstol(s.c_str(), &end, 10));
        if (end == s.c_str() || *end != L':') return false;
        const wchar_t* minutes = end + 1;
        m = static_cast<int>(wcstol(minutes, &end, 10));
        return end != minutes && *end == L'\0' && h >= 0 && h <= 23 && m >= 0 && m <= 59;
    }

    // The value as the file should have it, or why it can't be saved.
    bool Normalize(const Field& f, const std::wstring& text, std::wstring& value, std::wstring& error, bool uk)
    {
        wchar_t buf[256]{};
        value = Trim(text);
        if (value.empty())
        {
            if (!f.required) return true;
            error = uk ? L"Це поле не може бути порожнім." : L"This field can't be empty.";
            return false;
        }
        switch (f.kind)
        {
        case Kind::Int:
        case Kind::Float:
        {
            double v = 0;
            const bool whole = f.kind == Kind::Int;
            if (!ParseNumber(value, v) || (whole && v != std::floor(v)) || v < f.lo || v > f.hi)
            {
                swprintf_s(buf, uk ? (whole ? L"Потрібне ціле число від %g до %g." : L"Потрібне число від %g до %g.")
                                   : (whole ? L"Enter a whole number from %g to %g." : L"Enter a number from %g to %g."),
                           f.lo, f.hi);
                error = buf;
                return false;
            }
            swprintf_s(buf, whole ? L"%.0f" : L"%g", v);
            value = buf;
            return true;
        }
        case Kind::Time:
        {
            int h = 0, m = 0;
            if (!ParseTime(value, h, m))
            {
                error = uk ? L"Потрібен час у вигляді ГГ:ХХ, наприклад 07:30." : L"Enter a time as HH:MM, for example 07:30.";
                return false;
            }
            swprintf_s(buf, L"%02d:%02d", h, m);
            value = buf;
            return true;
        }
        default:
            break;
        }
        if (f.kind == Kind::Text && wcscmp(f.key, L"relay") == 0 &&
            _wcsnicmp(value.c_str(), L"https://", 8) != 0 && _wcsnicmp(value.c_str(), L"http://", 7) != 0)
        {
            error = uk ? L"Адреса має починатися з https://." : L"The address has to start with https://.";
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------ window
    constexpr UINT_PTR kFirstFieldId = 1000;
    constexpr int kListId = 100;

    // Layout at 96 DPI: page list on the left, one row per value to its
    // right, then the description of the focused value and the buttons.
    constexpr int kClientW = 900, kClientH = 660;
    constexpr int kMargin = 16, kListW = 178;
    constexpr int kBodyX = kMargin + kListW + 20, kBodyR = kClientW - kMargin;
    constexpr int kTitleY = 14, kNoteY = 50, kRowsY = 96, kRowH = 28, kControlH = 24;
    constexpr int kLabelW = 290, kControlX = kBodyX + kLabelW + 10;
    constexpr int kNumberW = 110;
    constexpr int kHintY = 510, kHintH = 84;
    constexpr int kButtonY = kClientH - 48, kButtonW = 120, kButtonH = 32;

    constexpr COLORREF kInk = RGB(31, 37, 71), kMuted = RGB(90, 96, 130), kErrInk = RGB(200, 40, 60),
                       kCard = RGB(241, 244, 251), kAccent = RGB(60, 110, 220);

    struct Row
    {
        const Field* field = nullptr;
        HWND         control = nullptr;
        std::wstring initial;
    };

    struct Window
    {
        const SettingsRequest* req = nullptr;
        SettingsResult result;
        HWND   hwnd = nullptr, list = nullptr, save = nullptr, cancel = nullptr;
        HFONT  ui = nullptr, title = nullptr;
        HBRUSH white = nullptr;
        UINT   dpi = 96;
        Row    rows[kFieldCount];
        int    page = 0;
        int    focused = -1;     // row the description belongs to
        std::wstring error;      // shown instead of the description
        bool   done = false;
    };
    Window* g_w = nullptr;       // the window is modal: one at a time

    int Px(int v) { return MulDiv(v, static_cast<int>(g_w->dpi), 96); }
    bool Uk() { return g_w->req->uk; }
    const wchar_t* Label(const Field& f) { return Uk() ? f.uk : f.en; }
    const wchar_t* Hint(const Field& f) { return Uk() ? f.hintUk : f.hintEn; }

    const std::wstring& PathOf(const Field& f)
    {
        return wcscmp(f.section, L"sounds") == 0 ? g_w->req->soundsPath : g_w->req->configPath;
    }

    // What the file says now; for a key it doesn't have, what the service
    // uses instead. Hand-edited numbers may carry a "; comment".
    std::wstring CurrentValue(const Field& f)
    {
        constexpr wchar_t kMissing[] = L"\x1F";
        wchar_t buf[2048]{};
        GetPrivateProfileStringW(f.section, f.key, kMissing, buf, static_cast<DWORD>(std::size(buf)), PathOf(f).c_str());
        if (wcscmp(buf, kMissing) == 0) return f.fallback ? f.fallback() : std::wstring();
        std::wstring v = buf;
        if (f.kind != Kind::Text && f.kind != Kind::Camera) v = v.substr(0, v.find(L';'));
        return Trim(v);
    }

    constexpr const wchar_t* kModes[] = { L"angle", L"floor", L"both" };
    const wchar_t* ModeName(int i)
    {
        static constexpr const wchar_t* kEn[] = { L"Knee angle", L"Hip height", L"Both at once" };
        static constexpr const wchar_t* kUk[] = { L"Кут коліна", L"Висота стегон", L"Обидва одразу" };
        return (Uk() ? kUk : kEn)[i];
    }

    std::wstring ControlValue(const Row& r)
    {
        if (r.field->kind == Kind::Bool)
            return SendMessageW(r.control, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0";
        if (r.field->kind == Kind::Mode)
        {
            const int i = static_cast<int>(SendMessageW(r.control, CB_GETCURSEL, 0, 0));
            return kModes[std::clamp(i, 0, static_cast<int>(std::size(kModes)) - 1)];
        }
        wchar_t buf[2048]{};
        GetWindowTextW(r.control, buf, static_cast<int>(std::size(buf)));
        return buf;
    }

    void SetControlValue(const Row& r, const std::wstring& value)
    {
        switch (r.field->kind)
        {
        case Kind::Bool:
            SendMessageW(r.control, BM_SETCHECK, _wtoi(value.c_str()) != 0 ? BST_CHECKED : BST_UNCHECKED, 0);
            break;
        case Kind::Mode:
            for (int i = 0; i < static_cast<int>(std::size(kModes)); ++i)
                if (i == 0 || _wcsicmp(value.c_str(), kModes[i]) == 0)
                    SendMessageW(r.control, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            break;
        default:
            SetWindowTextW(r.control, value.c_str());
            break;
        }
    }

    int RowsBefore(int index)
    {
        int n = 0;
        for (int i = 0; i < index; ++i)
            if (kFields[i].page == kFields[index].page) ++n;
        return n;
    }

    void CreateRows()
    {
        Window& w = *g_w;
        for (int i = 0; i < kFieldCount; ++i)
        {
            const Field& f = kFields[i];
            Row& r = w.rows[i];
            r.field = &f;

            const wchar_t* cls = WC_EDITW;
            DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, exStyle = WS_EX_CLIENTEDGE;
            int width = kBodyR - kControlX;
            int dropH = kControlH;
            switch (f.kind)
            {
            case Kind::Int:
            case Kind::Float:
            case Kind::Time:
                width = kNumberW;
                style |= ES_CENTER;
                break;
            case Kind::Bool:
                cls = WC_BUTTONW;
                style = WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY;
                exStyle = 0;
                width = kControlH;
                break;
            case Kind::Mode:
            case Kind::Camera:
                cls = WC_COMBOBOXW;
                style = WS_CHILD | WS_TABSTOP | WS_VSCROLL | (f.kind == Kind::Mode ? CBS_DROPDOWNLIST : CBS_DROPDOWN | CBS_AUTOHSCROLL);
                exStyle = 0;
                dropH = 200;
                break;
            default:
                break;
            }
            r.control = CreateWindowExW(exStyle, cls, L"", style, Px(kControlX), Px(kRowsY + RowsBefore(i) * kRowH),
                                        Px(width), Px(dropH), w.hwnd,
                                        reinterpret_cast<HMENU>(kFirstFieldId + static_cast<UINT_PTR>(i)), w.req->inst, nullptr);
            SendMessageW(r.control, WM_SETFONT, reinterpret_cast<WPARAM>(w.ui), FALSE);
            if (f.kind == Kind::Mode)
                for (int m = 0; m < static_cast<int>(std::size(kModes)); ++m)
                    SendMessageW(r.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ModeName(m)));
            if (f.kind == Kind::Camera)
                for (const std::wstring& name : w.req->cameras)
                    SendMessageW(r.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            SetControlValue(r, CurrentValue(f));
            r.initial = ControlValue(r);
        }
    }

    void ShowPage(int page)
    {
        Window& w = *g_w;
        w.page = page;
        w.focused = -1;
        w.error.clear();
        for (const Row& r : w.rows)
            ShowWindow(r.control, r.field->page == page ? SW_SHOW : SW_HIDE);
        SendMessageW(w.list, LB_SETCURSEL, static_cast<WPARAM>(page), 0);
        InvalidateRect(w.hwnd, nullptr, TRUE);
    }

    void ShowError(const Row& r, const std::wstring& message)
    {
        Window& w = *g_w;
        ShowPage(r.field->page);
        SetFocus(r.control);
        w.focused = static_cast<int>(&r - w.rows);
        w.error = message;
        InvalidateRect(w.hwnd, nullptr, TRUE);
    }

    const Row* FindRow(const wchar_t* section, const wchar_t* key)
    {
        for (const Row& r : g_w->rows)
            if (wcscmp(r.field->section, section) == 0 && wcscmp(r.field->key, key) == 0) return &r;
        return nullptr;
    }

    // Checks only what is being changed: a value nobody touched neither
    // blocks the save nor gets rewritten.
    bool CheckOrder(const Rule& rule, const std::vector<std::wstring>& values)
    {
        double previous = 0;
        const Row* first = nullptr;
        bool touched = false;
        for (const wchar_t* key : rule.keys)
        {
            const Row* r = key ? FindRow(rule.section, key) : nullptr;
            if (r) touched = touched || values[static_cast<size_t>(r - g_w->rows)] != r->initial;
        }
        if (!touched) return true;
        for (const wchar_t* key : rule.keys)
        {
            const Row* r = key ? FindRow(rule.section, key) : nullptr;
            if (!r) continue;
            double v = 0;
            ParseNumber(values[static_cast<size_t>(r - g_w->rows)], v);
            if (first && v >= previous)
            {
                const PageText& page = kPages[first->field->page];
                wchar_t buf[256]{};
                swprintf_s(buf, Uk() ? rule.whatUk : rule.whatEn, Uk() ? page.uk : page.en);
                ShowError(*first, buf);
                return false;
            }
            if (!first) first = r;
            previous = v;
        }
        return true;
    }

    bool SaveAll()
    {
        Window& w = *g_w;
        std::vector<std::wstring> values(kFieldCount);
        for (int i = 0; i < kFieldCount; ++i)
        {
            const Row& r = w.rows[i];
            const std::wstring text = ControlValue(r);
            if (text == r.initial) { values[static_cast<size_t>(i)] = text; continue; }
            std::wstring error;
            if (!Normalize(*r.field, text, values[static_cast<size_t>(i)], error, Uk()))
            {
                ShowError(r, error);
                return false;
            }
        }
        for (const Rule& rule : kRules)
            if (!CheckOrder(rule, values)) return false;

        bool anyConfig = false, anyValue = false;
        for (int i = 0; i < kFieldCount; ++i)
        {
            if (values[static_cast<size_t>(i)] == w.rows[i].initial) continue;
            anyValue = true;
            anyConfig = anyConfig || wcscmp(w.rows[i].field->section, L"sounds") != 0;
        }
        if (!anyValue) return true;

        if (anyConfig) CopyFileW(w.req->configPath.c_str(), (w.req->configPath + L".bak").c_str(), FALSE);
        for (int i = 0; i < kFieldCount; ++i)
        {
            const Row& r = w.rows[i];
            const Field& f = *r.field;
            const std::wstring& v = values[static_cast<size_t>(i)];
            if (v == r.initial) continue;
            // An empty text is a value of its own (no blocklist, no relay);
            // other empty fields mean "not set".
            const bool remove = v.empty() && f.kind != Kind::Text && f.kind != Kind::Camera;
            if (!WritePrivateProfileStringW(f.section, f.key, remove ? nullptr : v.c_str(), PathOf(f).c_str()))
            {
                wchar_t buf[256]{};
                swprintf_s(buf, Uk() ? L"Не вдалося записати %s (помилка %lu)." : L"Could not write %s (error %lu).",
                           PathOf(f).c_str(), GetLastError());
                ShowError(r, buf);
                return false;
            }
            LogF(L"calibrate: setting [%s] %s: '%s' -> '%s'", f.section, f.key, r.initial.c_str(), v.c_str());
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, w.req->configPath.c_str());   // flush
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, w.req->soundsPath.c_str());
        w.result.saved = true;
        w.result.serviceConfig = anyConfig;
        return true;
    }

    void Close()
    {
        Window& w = *g_w;
        w.done = true;
        // Before DestroyWindow, or the activation goes to another program.
        if (w.req->owner) EnableWindow(w.req->owner, TRUE);
        DestroyWindow(w.hwnd);
    }

    void DrawPageItem(const DRAWITEMSTRUCT& d)
    {
        if (d.itemID >= kPageCount) return;
        const bool selected = (d.itemState & ODS_SELECTED) != 0;
        HBRUSH bg = CreateSolidBrush(selected ? kCard : RGB(255, 255, 255));
        FillRect(d.hDC, &d.rcItem, bg);
        DeleteObject(bg);
        if (selected)
        {
            RECT bar{ d.rcItem.left, d.rcItem.top, d.rcItem.left + Px(3), d.rcItem.bottom };
            HBRUSH accent = CreateSolidBrush(kAccent);
            FillRect(d.hDC, &bar, accent);
            DeleteObject(accent);
        }
        RECT r = d.rcItem;
        r.left += Px(14);
        const PageText& page = kPages[d.itemID];
        HGDIOBJ old = SelectObject(d.hDC, g_w->ui);
        SetBkMode(d.hDC, TRANSPARENT);
        SetTextColor(d.hDC, selected ? kInk : kMuted);
        DrawTextW(d.hDC, Uk() ? page.uk : page.en, -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(d.hDC, old);
        if ((d.itemState & ODS_FOCUS) != 0 && (d.itemState & ODS_NOFOCUSRECT) == 0) DrawFocusRect(d.hDC, &d.rcItem);
    }

    void Paint(HDC dc, const RECT& client)
    {
        const Window& w = *g_w;
        FillRect(dc, &client, w.white);
        SetBkMode(dc, TRANSPARENT);

        RECT line{ Px(kBodyX - 10), Px(kTitleY), Px(kBodyX - 9), client.bottom - Px(kMargin) };
        HBRUSH lineBrush = CreateSolidBrush(kCard);
        FillRect(dc, &line, lineBrush);
        DeleteObject(lineBrush);

        const PageText& page = kPages[w.page];
        RECT r{ Px(kBodyX), Px(kTitleY), Px(kBodyR), Px(kNoteY) };
        SelectObject(dc, w.title);
        SetTextColor(dc, kInk);
        DrawTextW(dc, Uk() ? page.uk : page.en, -1, &r, DT_LEFT | DT_SINGLELINE);

        SelectObject(dc, w.ui);
        if (page.noteEn)
        {
            SetTextColor(dc, kMuted);
            r = RECT{ Px(kBodyX), Px(kNoteY), Px(kBodyR), Px(kRowsY - 4) };
            DrawTextW(dc, Uk() ? page.noteUk : page.noteEn, -1, &r, DT_LEFT | DT_WORDBREAK);
        }

        SetTextColor(dc, kInk);
        for (int i = 0; i < kFieldCount; ++i)
        {
            if (kFields[i].page != w.page) continue;
            const int y = kRowsY + RowsBefore(i) * kRowH;
            r = RECT{ Px(kBodyX), Px(y), Px(kBodyX + kLabelW), Px(y + kControlH) };
            DrawTextW(dc, Label(kFields[i]), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }

        const std::wstring hint = !w.error.empty() ? w.error
                                : w.focused >= 0 ? Hint(*w.rows[w.focused].field)
                                : Uk() ? L"Виберіть поле, щоб побачити його опис." : L"Pick a field to see what it does.";
        RECT card{ Px(kBodyX), Px(kHintY), Px(kBodyR), Px(kHintY + kHintH) };
        HBRUSH cardBrush = CreateSolidBrush(kCard);
        FillRect(dc, &card, cardBrush);
        DeleteObject(cardBrush);
        InflateRect(&card, -Px(12), -Px(8));
        if (w.focused >= 0)
        {
            RECT head = card;
            SetTextColor(dc, kInk);
            DrawTextW(dc, Label(*w.rows[w.focused].field), -1, &head, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            card.top += Px(22);
        }
        SetTextColor(dc, w.error.empty() ? kMuted : kErrInk);
        DrawTextW(dc, hint.c_str(), -1, &card, DT_LEFT | DT_WORDBREAK);
    }

    LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        Window* w = g_w;
        switch (msg)
        {
        case WM_CREATE:
        {
            w->hwnd = hwnd;
            auto font = [&](int pt, int weight) {
                return CreateFontW(-MulDiv(pt, static_cast<int>(w->dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, L"Segoe UI");
            };
            w->ui = font(10, FW_NORMAL);
            w->title = font(15, FW_SEMIBOLD);
            w->white = CreateSolidBrush(RGB(255, 255, 255));

            w->list = CreateWindowExW(0, WC_LISTBOXW, L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                      Px(kMargin), Px(kTitleY), Px(kListW), Px(kClientH - kTitleY - kMargin), hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), w->req->inst, nullptr);
            SendMessageW(w->list, WM_SETFONT, reinterpret_cast<WPARAM>(w->ui), FALSE);
            for (const PageText& p : kPages)
                SendMessageW(w->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w->req->uk ? p.uk : p.en));

            CreateRows();

            auto button = [&](int id, const wchar_t* text, int x, DWORD style) {
                HWND b = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                         Px(x), Px(kButtonY), Px(kButtonW), Px(kButtonH), hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), w->req->inst, nullptr);
                SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(w->ui), FALSE);
                return b;
            };
            w->cancel = button(IDCANCEL, w->req->uk ? L"Скасувати" : L"Cancel", kBodyR - 2 * kButtonW - 8, BS_PUSHBUTTON);
            w->save = button(IDOK, w->req->uk ? L"Зберегти" : L"Save", kBodyR - kButtonW, BS_DEFPUSHBUTTON);
            ShowPage(0);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC wdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC dc = CreateCompatibleDC(wdc);
            HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(dc, bmp);
            Paint(dc, rc);
            BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            return reinterpret_cast<LRESULT>(w->white);   // the checkboxes' background
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = static_cast<UINT>(Px(34));
            return TRUE;
        case WM_DRAWITEM:
            DrawPageItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_COMMAND:
        {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDOK) { if (SaveAll()) Close(); return 0; }
            if (id == IDCANCEL) { Close(); return 0; }
            if (id == kListId && code == LBN_SELCHANGE)
                ShowPage(static_cast<int>(SendMessageW(w->list, LB_GETCURSEL, 0, 0)));
            else if (id >= static_cast<int>(kFirstFieldId) && id < static_cast<int>(kFirstFieldId) + kFieldCount &&
                     (code == EN_SETFOCUS || code == CBN_SETFOCUS || code == BN_SETFOCUS))
            {
                if (w->focused != id - static_cast<int>(kFirstFieldId)) w->error.clear();
                w->focused = id - static_cast<int>(kFirstFieldId);
                RECT card{ Px(kBodyX), Px(kHintY), Px(kBodyR), Px(kHintY + kHintH) };
                InvalidateRect(hwnd, &card, FALSE);
            }
            return 0;
        }
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
            DeleteObject(w->ui);
            DeleteObject(w->title);
            DeleteObject(w->white);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
}

    SettingsResult ShowSettings(const SettingsRequest& request)
    {
        Window window;
        window.req = &request;
        g_w = &window;
        const bool snapshot = !request.snapshotPath.empty();

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = request.inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ExerciseUnlockSettings";
        wc.hIcon = static_cast<HICON>(LoadImageW(request.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        wc.hIconSm = static_cast<HICON>(LoadImageW(request.inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        RegisterClassExW(&wc);

        window.dpi = request.owner ? GetDpiForWindow(request.owner) : GetDpiForSystem();
        constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        RECT size{ 0, 0, Px(kClientW), Px(kClientH) };
        AdjustWindowRectExForDpi(&size, kStyle, FALSE, 0, window.dpi);
        const int width = size.right - size.left, height = size.bottom - size.top;

        // Centred over the wizard, or the screen when there is none.
        RECT around{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (request.owner) GetWindowRect(request.owner, &around);
        const int x = snapshot ? -6000 : (around.left + around.right - width) / 2;
        const int y = snapshot ? -6000 : std::max(0L, (around.top + around.bottom - height) / 2);

        const wchar_t* title = request.uk ? L"Exercise Unlock - налаштування" : L"Exercise Unlock - settings";
        HWND hwnd = CreateWindowExW(snapshot ? WS_EX_TOOLWINDOW : 0, wc.lpszClassName, title, kStyle,
                                    x, y, width, height, request.owner, nullptr, request.inst, nullptr);
        if (!hwnd)
        {
            g_w = nullptr;
            return {};
        }

        if (snapshot)
        {
            ShowPage(std::clamp(request.snapshotPage, 0, kPageCount - 1));
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            WriteWindowBmp(hwnd, request.snapshotPath, [](HDC dc, const RECT& rc) { Paint(dc, rc); });
            DestroyWindow(hwnd);
            g_w = nullptr;
            return {};
        }

        if (request.owner) EnableWindow(request.owner, FALSE);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        SetFocus(window.list);

        MSG m{};
        while (!window.done)
        {
            const BOOL got = GetMessageW(&m, nullptr, 0, 0);
            if (got <= 0)
            {
                // Quitting: leave the message for the wizard's own loop.
                if (got == 0) PostQuitMessage(static_cast<int>(m.wParam));
                break;
            }
            if (!IsDialogMessageW(hwnd, &m))
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        g_w = nullptr;
        return window.result;
    }
}
