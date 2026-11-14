#include "Config.h"
#include "Log.h"

#include <shlwapi.h>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <climits>
#include <ctime>

#pragma comment(lib, "shlwapi.lib")

namespace ExerciseUnlock::Svc
{
    // Defaults match the built-in "20 push-ups OR 60 squats = 10 min"
    // rule described in the design doc, so a missing / broken
    // config.ini still gives a reasonable experience.
    std::atomic<int> Config::s_pushupSec   { 30 };
    std::atomic<int> Config::s_squatSec    { 10 };
    // Sit-ups came later (1.3): this default also applies to an update
    // that keeps an older config.ini without situp_seconds.
    std::atomic<int> Config::s_situpSec    { 300 };
    std::atomic<int> Config::s_minUnlockSec{ 600 };
    // Hard cap on a single unlock (seconds). 3600 = 1 hour.
    std::atomic<int> Config::s_maxSessionSec{ 3600 };
    // Grace: 5 minutes after session end, Submit grants 60-sec unlock.
    std::atomic<int> Config::s_graceWindowSec{ 300 };
    std::atomic<int> Config::s_graceUnlockSec{ 60 };

    // Squat detector defaults — match previously-hard-coded constants.
    std::atomic<float> Config::s_squatStandingAng  { 170.0f };
    std::atomic<float> Config::s_squatDownAng      { 120.0f };
    std::atomic<float> Config::s_squatGoingDownAng { 160.0f };
    std::atomic<float> Config::s_squatGoingUpAng   { 130.0f };
    std::atomic<int>   Config::s_squatMinRepMs     { 1000 };
    std::atomic<int>   Config::s_squatMaxRepMs     { 2000 };

    // Default: angle mode (keep existing behaviour).
    std::atomic<int>   Config::s_squatMode         { 0 };
    std::atomic<float> Config::s_squatStandHipFrac { 0.40f };
    std::atomic<float> Config::s_squatDownHipFrac  { 0.15f };
    std::atomic<float> Config::s_squatGoDownHipFrac{ 0.35f };
    std::atomic<float> Config::s_squatGoUpHipFrac  { 0.20f };

    // Push-up detector defaults — match previously-hard-coded constants.
    std::atomic<float> Config::s_pushupUpAng        { 165.0f };
    std::atomic<float> Config::s_pushupDownAng      { 155.0f };
    std::atomic<float> Config::s_pushupGoingDownAng { 162.0f };
    std::atomic<float> Config::s_pushupGoingUpAng   { 158.0f };
    std::atomic<float> Config::s_pushupPlankMinAng  { 140.0f };
    std::atomic<int>   Config::s_pushupMinRepMs     { 1000 };
    std::atomic<int>   Config::s_pushupMaxRepMs     { 2000 };

    // Sit-up defaults (uncalibrated): lying on the back with knees bent
    // the hip angle is ~130-140 deg, sitting up ~50-70 - but measured on
    // x / width, y / height of a 16:9 frame lying reads ~120 and up ~35.
    // Forgiving values until the calibration measures the child. Slower
    // reps than squats, so a wider timing window.
    std::atomic<float> Config::s_situpLyingAng      { 105.0f };
    std::atomic<float> Config::s_situpRisingAng     {  95.0f };
    std::atomic<float> Config::s_situpLoweringAng   {  75.0f };
    std::atomic<float> Config::s_situpSittingAng    {  60.0f };
    std::atomic<int>   Config::s_situpMinRepMs      { 800 };
    std::atomic<int>   Config::s_situpMaxRepMs      { 6000 };
    std::atomic<float> Config::s_situpQualityMin    { 0.0f };

    std::map<std::wstring, UserRules> Config::s_rules;

    // Progression disabled by default.
    std::atomic<float> Config::s_progressionFactor  { 0.0f };
    std::atomic<int>   Config::s_progressionDays    { 0 };

    // Form-quality gating disabled by default (0 = no gate).
    std::atomic<float> Config::s_squatQualityMin    { 0.0f };
    std::atomic<float> Config::s_pushupQualityMin   { 0.0f };

    // Hip-drift gate on by default: 0.12 of frame width catches
    // obvious walk-across-the-frame false positives while leaving
    // real squats (hip wiggles maybe 2-3% during the up-down cycle)
    // alone. Set to 0 in config.ini to disable.
    std::atomic<float> Config::s_squatMaxHipDrift   { 0.12f };

    // Knee-lift gate on by default: require the hip to actually
    // drop by at least this fraction of frame height during a
    // "squat". Adult observed drop on a real rep is 0.10..0.20;
    // a single-leg knee lift stays <0.03. 0.07 is a safe midpoint
    // that admits even shallow kid-squats while rejecting lifts.
    std::atomic<float> Config::s_squatMinHipDrop    { 0.07f };

    // Common Windows virtual-camera friendly-name substrings.
    std::wstring       Config::s_vcamBlocklist{
        L"OBS Virtual Camera,XSplit,ManyCam,Snap Camera,SplitCam,e2eSoft,vMix,Virtual Camera,DroidCam" };
    std::atomic<int>   Config::s_livenessMaxStatic  { 30 };  // ~1s at 30 fps

    std::atomic<bool>  Config::s_soundOnRep         { true };

    std::atomic<bool>  Config::s_ignoreSchedule     { false };

    // Camera only on the lock / sign-in screen by default.
    std::atomic<bool>  Config::s_cameraLockScreenOnly{ true };

    // On by default with the project's relay, so an update that keeps an
    // older config.ini (no [phone] section) gets the phone too.
    constexpr wchar_t  kDefaultRelayUrl[] = L"https://exercise-unlock.sdv291.com";
    std::atomic<bool>  Config::s_phoneEnabled       { true };
    std::wstring       Config::s_phoneRelayUrl      { kDefaultRelayUrl };
    std::atomic<float> Config::s_calibrationAspect  { 0.0f };
    Config::PhoneAngles Config::s_phoneAngles[3];
    float              Config::s_phoneHipDrop       { -1.0f };

    constexpr wchar_t  kDefaultManifestUrl[] = L"https://exercise-unlock.sdv291.com/download/version.txt";
    std::atomic<bool>  Config::s_updateEnabled      { true };
    std::atomic<int>   Config::s_updateIntervalH    { 24 };
    std::wstring       Config::s_updateUrl          { kDefaultManifestUrl };

    std::mutex       Config::s_stringsMx;
    std::wstring     Config::s_cameraPref;
    std::wstring     Config::s_lockInMessage{ L"Computer will lock in {min} minutes" };
    std::wstring     Config::s_labelSquats  { L"Squats" };
    std::wstring     Config::s_labelPushups { L"Push-ups" };
    std::wstring     Config::s_labelSitups  { L"Sit-ups" };
    std::wstring     Config::s_labelEarned  { L"Earned" };
    std::wstring     Config::s_labelReady   { L"— ready to unlock" };
    std::wstring     Config::s_labelOutsideHours{ L"Outside allowed hours ({from}-{to})" };

    std::wstring Config::CameraPreference()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_cameraPref;
    }
    std::wstring Config::LockInMessage()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_lockInMessage;
    }
    std::wstring Config::LabelSquats()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_labelSquats;
    }
    std::wstring Config::LabelPushups()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_labelPushups;
    }
    std::wstring Config::LabelSitups()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_labelSitups;
    }
    std::wstring Config::LabelEarned()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_labelEarned;
    }
    std::wstring Config::LabelReady()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_labelReady;
    }
    std::wstring Config::LabelOutsideHours()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_labelOutsideHours;
    }

    static bool IsWeekendNow()
    {
        SYSTEMTIME lt{};
        GetLocalTime(&lt);
        // wDayOfWeek: 0=Sun, 6=Sat
        return lt.wDayOfWeek == 0 || lt.wDayOfWeek == 6;
    }

    int UserRules::FromToday() const      { return fromMin[IsWeekendNow() ? 1 : 0]; }
    int UserRules::ToToday() const        { return toMin[IsWeekendNow() ? 1 : 0]; }
    int UserRules::MinUnlockToday() const { return minUnlockSec[IsWeekendNow() ? 1 : 0]; }

    bool UserRules::InWindowNow() const
    {
        if (Config::IgnoringSchedule()) return true;
        SYSTEMTIME lt{};
        GetLocalTime(&lt);
        const int nowMin = lt.wHour * 60 + lt.wMinute;
        const int from = FromToday();
        const int to   = ToToday();
        if (from == to) return true;
        if (from < to)  return nowMin >= from && nowMin < to;
        return nowMin >= from || nowMin < to;
    }

    std::wstring Config::VirtualCameraBlocklist()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_vcamBlocklist;
    }

    std::wstring Config::UpdateManifestUrl()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_updateUrl;
    }

    std::wstring Config::PhoneRelayUrl()
    {
        std::lock_guard<std::mutex> lk(s_stringsMx);
        return s_phoneRelayUrl;
    }

    SquatParams Config::Squat()
    {
        SquatParams p;
        p.standing         = SquatStandingAngle();
        p.down             = SquatDownAngle();
        p.goingDown        = SquatGoingDownAngle();
        p.goingUp          = SquatGoingUpAngle();
        p.minRepMs         = SquatMinRepMs();
        p.maxRepMs         = SquatMaxRepMs();
        p.qualityMin       = SquatQualityMin();
        p.maxHipDrift      = SquatMaxHipDrift();
        p.minHipDrop       = SquatMinHipDrop();
        p.mode             = SquatDetectorMode();
        p.standingHipFrac  = SquatStandingHipFrac();
        p.downHipFrac      = SquatDownHipFrac();
        p.goingDownHipFrac = SquatGoingDownHipFrac();
        p.goingUpHipFrac   = SquatGoingUpHipFrac();
        return p;
    }

    PushupParams Config::Pushup()
    {
        PushupParams p;
        p.up         = PushupUpAngle();
        p.down       = PushupDownAngle();
        p.goingDown  = PushupGoingDownAngle();
        p.goingUp    = PushupGoingUpAngle();
        p.plankMin   = PushupPlankMinAngle();
        p.minRepMs   = PushupMinRepMs();
        p.maxRepMs   = PushupMaxRepMs();
        p.qualityMin = PushupQualityMin();
        return p;
    }

    SitupParams Config::Situp()
    {
        SitupParams p;
        p.lying      = SitupLyingAngle();
        p.rising     = SitupRisingAngle();
        p.lowering   = SitupLoweringAngle();
        p.sitting    = SitupSittingAngle();
        p.minRepMs   = SitupMinRepMs();
        p.maxRepMs   = SitupMaxRepMs();
        p.qualityMin = SitupQualityMin();
        return p;
    }

    bool Config::PhoneProfile(SquatParams& squat, PushupParams& pushup, SitupParams& situp)
    {
        squat = Squat();
        pushup = Pushup();
        situp = Situp();
        std::lock_guard<std::mutex> lk(s_stringsMx);
        if (const PhoneAngles& s = s_phoneAngles[0]; s.have)
        {
            squat.standing = s.a[0]; squat.goingDown = s.a[1]; squat.goingUp = s.a[2]; squat.down = s.a[3];
        }
        if (s_phoneHipDrop >= 0.0f) squat.minHipDrop = s_phoneHipDrop;
        if (const PhoneAngles& s = s_phoneAngles[1]; s.have)
        {
            pushup.up = s.a[0]; pushup.goingDown = s.a[1]; pushup.goingUp = s.a[2]; pushup.down = s.a[3];
        }
        if (const PhoneAngles& s = s_phoneAngles[2]; s.have)
        {
            situp.lying = s.a[0]; situp.rising = s.a[1]; situp.lowering = s.a[2]; situp.sitting = s.a[3];
        }
        return s_phoneAngles[0].have || s_phoneAngles[1].have || s_phoneAngles[2].have;
    }

    static std::wstring FindConfigPath()
    {
        wchar_t exe[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
        PathRemoveFileSpecW(exe);
        std::wstring p = exe;
        p += L"\\config.ini";
        return p;
    }

    namespace
    {
        // One key of config.ini as written, without a "; comment"; false
        // when the section doesn't have it (or it is empty).
        bool ReadKey(const std::wstring& path, const std::wstring& section, const wchar_t* key, std::wstring& out)
        {
            constexpr wchar_t kMissing[] = L"\x1F";
            wchar_t buf[256]{};
            GetPrivateProfileStringW(section.c_str(), key, kMissing, buf, static_cast<DWORD>(std::size(buf)), path.c_str());
            if (wcscmp(buf, kMissing) == 0) return false;
            std::wstring v = buf;
            v = v.substr(0, v.find(L';'));
            const size_t a = v.find_first_not_of(L" \t");
            if (a == std::wstring::npos) return false;
            out = v.substr(a, v.find_last_not_of(L" \t") - a + 1);
            return true;
        }

        bool ParseInt(const std::wstring& s, int& out)
        {
            wchar_t* end = nullptr;
            const long v = wcstol(s.c_str(), &end, 10);
            if (end == s.c_str() || *end != L'\0') return false;
            out = static_cast<int>(v);
            return true;
        }

        bool ParseFloat(const std::wstring& s, float& out)
        {
            wchar_t* end = nullptr;
            const double v = wcstod(s.c_str(), &end);
            if (end == s.c_str() || *end != L'\0') return false;
            out = static_cast<float>(v);
            return true;
        }

        bool ParseHHMM(const std::wstring& s, int& out)
        {
            wchar_t* end = nullptr;
            const long h = wcstol(s.c_str(), &end, 10);
            if (end == s.c_str() || *end != L':') return false;
            const wchar_t* m0 = end + 1;
            const long m = wcstol(m0, &end, 10);
            if (end == m0 || *end != L'\0' || h < 0 || h > 23 || m < 0 || m > 59) return false;
            out = static_cast<int>(h * 60 + m);
            return true;
        }

        // Where an account's value comes from, first match wins: the
        // account's own sections, then the common ones. Within one layer
        // the keys are tried in order (e.g. weekday_from, then active_from)
        // - an account that sets only active_from gets it on weekdays too,
        // even when the common section has its own weekday_from.
        struct Source { const wchar_t* section; const wchar_t* key; };

        template <typename T, typename Parse>
        T Pick(const std::wstring& path, const std::wstring& account, std::initializer_list<Source> keys,
               T fallback, Parse parse)
        {
            std::wstring raw;
            T value{};
            for (int layer = account.empty() ? 1 : 0; layer < 2; ++layer)
                for (const Source& s : keys)
                {
                    const std::wstring section = layer == 0 ? std::wstring(s.section) + L"@" + account : s.section;
                    if (ReadKey(path, section, s.key, raw) && parse(raw, value)) return value;
                }
            return fallback;
        }

        UserRules ReadRules(const std::wstring& path, const std::wstring& account)
        {
            UserRules r;
            auto i = [&](std::initializer_list<Source> keys, int fallback) { return Pick<int>(path, account, keys, fallback, ParseInt); };
            auto f = [&](std::initializer_list<Source> keys, float fallback) { return Pick<float>(path, account, keys, fallback, ParseFloat); };
            auto t = [&](std::initializer_list<Source> keys, int fallback) { return Pick<int>(path, account, keys, fallback, ParseHHMM); };
            auto positive = [](int v, int fallback) { return v > 0 ? v : fallback; };

            r.pushupSec = positive(i({ { L"reward", L"pushup_seconds" } }, r.pushupSec), 30);
            r.squatSec  = positive(i({ { L"reward", L"squat_seconds" } }, r.squatSec), 10);
            r.situpSec  = positive(i({ { L"reward", L"situp_seconds" } }, r.situpSec), 300);
            r.maxSessionSec  = (std::max)(0, i({ { L"reward", L"max_session_seconds" } }, r.maxSessionSec));
            r.graceWindowSec = (std::max)(0, i({ { L"reward", L"grace_window_seconds" } }, r.graceWindowSec));
            r.graceUnlockSec = positive(i({ { L"reward", L"grace_unlock_seconds" } }, r.graceUnlockSec), 60);
            r.progressionDays   = (std::max)(0, i({ { L"reward", L"progression_after_days" } }, 0));
            r.progressionFactor = (std::max)(0.0f, f({ { L"reward", L"progression_factor" } }, 0.0f));

            r.fromMin[0] = t({ { L"schedule", L"weekday_from" }, { L"schedule", L"active_from" } }, 7 * 60);
            r.toMin[0]   = t({ { L"schedule", L"weekday_to" },   { L"schedule", L"active_to" } },   20 * 60);
            r.fromMin[1] = t({ { L"schedule", L"weekend_from" }, { L"schedule", L"active_from" } }, 7 * 60);
            r.toMin[1]   = t({ { L"schedule", L"weekend_to" },   { L"schedule", L"active_to" } },   20 * 60);
            r.minUnlockSec[0] = positive(i({ { L"schedule", L"weekday_min_unlock_seconds" }, { L"reward", L"min_unlock_seconds" } }, 600), 600);
            r.minUnlockSec[1] = positive(i({ { L"schedule", L"weekend_min_unlock_seconds" }, { L"reward", L"min_unlock_seconds" } }, 600), 600);
            return r;
        }
    }

    UserRules Config::Rules(const std::wstring& account)
    {
        std::wstring key = account;
        for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
        {
            std::lock_guard<std::mutex> lk(s_stringsMx);
            if (auto it = s_rules.find(key); it != s_rules.end()) return it->second;
        }
        const UserRules r = ReadRules(FindConfigPath(), account);
        LogF(L"config: rules for '%s': squat=%d pushup=%d situp=%d s, min_unlock=%d/%d s, max=%d s, "
             L"hours %02d:%02d-%02d:%02d / %02d:%02d-%02d:%02d",
             account.empty() ? L"(everyone)" : account.c_str(), r.squatSec, r.pushupSec, r.situpSec,
             r.minUnlockSec[0], r.minUnlockSec[1], r.maxSessionSec,
             r.fromMin[0] / 60, r.fromMin[0] % 60, r.toMin[0] / 60, r.toMin[0] % 60,
             r.fromMin[1] / 60, r.fromMin[1] % 60, r.toMin[1] / 60, r.toMin[1] % 60);
        std::lock_guard<std::mutex> lk(s_stringsMx);
        s_rules[key] = r;
        return r;
    }

    void Config::Load()
    {
        const std::wstring path = FindConfigPath();
        if (path.empty() || !PathFileExistsW(path.c_str()))
        {
            LogF(L"config: %s not found, using defaults "
                 L"(pushup=%d s, squat=%d s, min_unlock=%d s)",
                 path.c_str(), s_pushupSec.load(), s_squatSec.load(),
                 s_minUnlockSec.load());
            return;
        }

        const int pu  = GetPrivateProfileIntW(
            L"reward", L"pushup_seconds",
            s_pushupSec.load(), path.c_str());
        const int sq  = GetPrivateProfileIntW(
            L"reward", L"squat_seconds",
            s_squatSec.load(), path.c_str());
        const int su  = GetPrivateProfileIntW(
            L"reward", L"situp_seconds",
            s_situpSec.load(), path.c_str());
        const int min = GetPrivateProfileIntW(
            L"reward", L"min_unlock_seconds",
            s_minUnlockSec.load(), path.c_str());
        s_maxSessionSec.store(GetPrivateProfileIntW(
            L"reward", L"max_session_seconds",
            s_maxSessionSec.load(), path.c_str()));
        s_graceWindowSec.store(GetPrivateProfileIntW(
            L"reward", L"grace_window_seconds",
            s_graceWindowSec.load(), path.c_str()));
        s_graceUnlockSec.store(GetPrivateProfileIntW(
            L"reward", L"grace_unlock_seconds",
            s_graceUnlockSec.load(), path.c_str()));

        // Guard against zero or negative values that would break the
        // formula. Fall back to the compiled defaults on garbage input.
        s_pushupSec.store   (pu  > 0 ? pu  : 30);
        s_squatSec.store    (sq  > 0 ? sq  : 10);
        s_situpSec.store    (su  > 0 ? su  : 300);
        s_minUnlockSec.store(min > 0 ? min : 600);

        auto readStr = [&](const wchar_t* section, const wchar_t* key,
                           const wchar_t* dflt) -> std::wstring
        {
            wchar_t buf[512]{};
            DWORD n = GetPrivateProfileStringW(
                section, key, dflt,
                buf, static_cast<DWORD>(std::size(buf)), path.c_str());
            std::wstring s(buf, n);
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' ||
                                  s.back() == L'\r' || s.back() == L'\n'))
            {
                s.pop_back();
            }
            return s;
        };

        // GetPrivateProfileIntW is int-only, so pull floats via string.
        // On empty / unparsable input we keep the current default.
        auto readFloat = [&](const wchar_t* section, const wchar_t* key,
                             float dflt) -> float
        {
            wchar_t sentinel[] = L"__UNSET__";
            wchar_t buf[64]{};
            GetPrivateProfileStringW(section, key, sentinel,
                buf, static_cast<DWORD>(std::size(buf)), path.c_str());
            if (wcscmp(buf, sentinel) == 0) return dflt;
            wchar_t* end = nullptr;
            const double v = wcstod(buf, &end);
            if (end == buf) return dflt;
            return static_cast<float>(v);
        };

        // --- squat thresholds + timing ---
        const float sqStand   = readFloat(L"squat", L"standing_angle",  s_squatStandingAng.load());
        const float sqDown    = readFloat(L"squat", L"down_angle",      s_squatDownAng.load());
        const float sqGDown   = readFloat(L"squat", L"going_down_angle",s_squatGoingDownAng.load());
        const float sqGUp     = readFloat(L"squat", L"going_up_angle",  s_squatGoingUpAng.load());
        const int   sqMinMs   = GetPrivateProfileIntW(L"squat", L"min_rep_ms",
                                                      s_squatMinRepMs.load(), path.c_str());
        const int   sqMaxMs   = GetPrivateProfileIntW(L"squat", L"max_rep_ms",
                                                      s_squatMaxRepMs.load(), path.c_str());
        s_squatStandingAng.store(sqStand);
        s_squatDownAng.store    (sqDown);
        s_squatGoingDownAng.store(sqGDown);
        s_squatGoingUpAng.store (sqGUp);
        s_squatMinRepMs.store   (sqMinMs > 0 ? sqMinMs : 1000);
        s_squatMaxRepMs.store   (sqMaxMs > sqMinMs ? sqMaxMs : (sqMinMs > 0 ? sqMinMs * 2 : 2000));

        // Floor-mode (v1.1) — accept 'mode = angle|floor|both'.
        const std::wstring sqModeStr = readStr(L"squat", L"mode", L"angle");
        int sqMode = 0;
        if (_wcsicmp(sqModeStr.c_str(), L"floor") == 0) sqMode = 1;
        else if (_wcsicmp(sqModeStr.c_str(), L"both") == 0) sqMode = 2;
        s_squatMode.store(sqMode);
        s_squatStandHipFrac.store (readFloat(L"squat", L"standing_hip_frac",  s_squatStandHipFrac.load()));
        s_squatDownHipFrac.store  (readFloat(L"squat", L"down_hip_frac",      s_squatDownHipFrac.load()));
        s_squatGoDownHipFrac.store(readFloat(L"squat", L"going_down_hip_frac",s_squatGoDownHipFrac.load()));
        s_squatGoUpHipFrac.store  (readFloat(L"squat", L"going_up_hip_frac",  s_squatGoUpHipFrac.load()));

        // --- pushup thresholds + timing ---
        const float puUp      = readFloat(L"pushup", L"up_angle",        s_pushupUpAng.load());
        const float puDown    = readFloat(L"pushup", L"down_angle",      s_pushupDownAng.load());
        const float puGDown   = readFloat(L"pushup", L"going_down_angle",s_pushupGoingDownAng.load());
        const float puGUp     = readFloat(L"pushup", L"going_up_angle",  s_pushupGoingUpAng.load());
        const float puPlank   = readFloat(L"pushup", L"plank_min_angle", s_pushupPlankMinAng.load());
        const int   puMinMs   = GetPrivateProfileIntW(L"pushup", L"min_rep_ms",
                                                      s_pushupMinRepMs.load(), path.c_str());
        const int   puMaxMs   = GetPrivateProfileIntW(L"pushup", L"max_rep_ms",
                                                      s_pushupMaxRepMs.load(), path.c_str());
        s_pushupUpAng.store       (puUp);
        s_pushupDownAng.store     (puDown);
        s_pushupGoingDownAng.store(puGDown);
        s_pushupGoingUpAng.store  (puGUp);
        s_pushupPlankMinAng.store (puPlank);
        s_pushupMinRepMs.store    (puMinMs > 0 ? puMinMs : 1000);
        s_pushupMaxRepMs.store    (puMaxMs > puMinMs ? puMaxMs : (puMinMs > 0 ? puMinMs * 2 : 2000));

        // --- sit-up thresholds + timing ---
        s_situpLyingAng.store   (readFloat(L"situp", L"lying_angle",    s_situpLyingAng.load()));
        s_situpRisingAng.store  (readFloat(L"situp", L"rising_angle",   s_situpRisingAng.load()));
        s_situpLoweringAng.store(readFloat(L"situp", L"lowering_angle", s_situpLoweringAng.load()));
        s_situpSittingAng.store (readFloat(L"situp", L"sitting_angle",  s_situpSittingAng.load()));
        const int   suMinMs   = GetPrivateProfileIntW(L"situp", L"min_rep_ms",
                                                      s_situpMinRepMs.load(), path.c_str());
        const int   suMaxMs   = GetPrivateProfileIntW(L"situp", L"max_rep_ms",
                                                      s_situpMaxRepMs.load(), path.c_str());
        s_situpMinRepMs.store(suMinMs > 0 ? suMinMs : 800);
        s_situpMaxRepMs.store(suMaxMs > suMinMs ? suMaxMs : (suMinMs > 0 ? suMinMs * 2 : 6000));

        // [schedule] and each account's own rules: Rules(), read on demand.
        {
            std::lock_guard<std::mutex> lk(s_stringsMx);
            s_rules.clear();
        }

        // Progression [reward] section, optional keys.
        s_progressionDays.store(GetPrivateProfileIntW(
            L"reward", L"progression_after_days", s_progressionDays.load(), path.c_str()));
        s_progressionFactor.store(readFloat(L"reward", L"progression_factor",
                                            s_progressionFactor.load()));

        // Quality gates.
        s_squatQualityMin.store (readFloat(L"squat",  L"quality_min", s_squatQualityMin.load()));
        s_pushupQualityMin.store(readFloat(L"pushup", L"quality_min", s_pushupQualityMin.load()));
        s_situpQualityMin.store (readFloat(L"situp",  L"quality_min", s_situpQualityMin.load()));
        s_squatMaxHipDrift.store(readFloat(L"squat",  L"max_hip_drift",
                                           s_squatMaxHipDrift.load()));
        s_squatMinHipDrop.store (readFloat(L"squat",  L"min_hip_drop",
                                           s_squatMinHipDrop.load()));

        // Bypass detection [security] section.
        const std::wstring vcam = readStr(L"security", L"virtual_camera_blocklist",
                                          L"OBS Virtual Camera,XSplit,ManyCam,Snap Camera,SplitCam,e2eSoft,vMix,Virtual Camera,DroidCam");
        s_livenessMaxStatic.store(GetPrivateProfileIntW(
            L"security", L"liveness_max_static_frames",
            s_livenessMaxStatic.load(), path.c_str()));

        // Sound feedback [ux] section.
        s_soundOnRep.store(GetPrivateProfileIntW(
            L"ux", L"sound_on_rep", s_soundOnRep.load() ? 1 : 0, path.c_str()) != 0);

        // Update checker [update] section.
        s_updateEnabled.store(GetPrivateProfileIntW(
            L"update", L"enabled", s_updateEnabled.load() ? 1 : 0, path.c_str()) != 0);
        s_updateIntervalH.store(GetPrivateProfileIntW(
            L"update", L"check_interval_hours", s_updateIntervalH.load(), path.c_str()));
        const std::wstring updUrl = readStr(L"update", L"manifest_url", kDefaultManifestUrl);

        // Phone camera [phone] + calibration geometry [calibration].
        s_phoneEnabled.store(GetPrivateProfileIntW(
            L"phone", L"enabled", s_phoneEnabled.load() ? 1 : 0, path.c_str()) != 0);
        const std::wstring relayUrl = readStr(L"phone", L"relay", kDefaultRelayUrl);
        s_calibrationAspect.store(readFloat(L"calibration", L"aspect", s_calibrationAspect.load()));

        // The phone's own calibration: an exercise counts as calibrated
        // when all four of its angles are there.
        PhoneAngles phone[3];
        {
            static const wchar_t* const kSections[3] = { L"phone_squat", L"phone_pushup", L"phone_situp" };
            static const wchar_t* const kKeys[3][4] = {
                { L"standing_angle", L"going_down_angle", L"going_up_angle", L"down_angle" },
                { L"up_angle",       L"going_down_angle", L"going_up_angle", L"down_angle" },
                { L"lying_angle",    L"rising_angle",     L"lowering_angle", L"sitting_angle" },
            };
            for (int ex = 0; ex < 3; ++ex)
            {
                phone[ex].have = true;
                for (int i = 0; i < 4; ++i)
                {
                    phone[ex].a[i] = readFloat(kSections[ex], kKeys[ex][i], -1.0f);
                    if (phone[ex].a[i] <= 0.0f) phone[ex].have = false;
                }
            }
        }
        const float phoneHipDrop = readFloat(L"phone_squat", L"min_hip_drop", -1.0f);

        {
            std::lock_guard<std::mutex> lk(s_stringsMx);
            s_vcamBlocklist = vcam;
            s_updateUrl     = updUrl;
            s_phoneRelayUrl = relayUrl;
            for (int ex = 0; ex < 3; ++ex) s_phoneAngles[ex] = phone[ex];
            s_phoneHipDrop = phoneHipDrop;
        }

        s_cameraLockScreenOnly.store(GetPrivateProfileIntW(
            L"camera", L"lock_screen_only",
            s_cameraLockScreenOnly.load() ? 1 : 0, path.c_str()) != 0);

        std::wstring cam     = readStr(L"camera",   L"preferred",       L"");
        std::wstring lockMsg = readStr(L"warnings", L"lock_in_message",
                                       L"Computer will lock in {min} minutes");
        std::wstring lSq  = readStr(L"labels", L"squats",  L"Squats");
        std::wstring lPu  = readStr(L"labels", L"pushups", L"Push-ups");
        std::wstring lSu  = readStr(L"labels", L"situps",  L"Sit-ups");
        std::wstring lErn = readStr(L"labels", L"earned",  L"Earned");
        std::wstring lRdy = readStr(L"labels", L"ready",   L"— ready to unlock");
        std::wstring lOut = readStr(L"labels", L"outside_hours",
                                    L"Outside allowed hours ({from}-{to})");
        {
            std::lock_guard<std::mutex> lk(s_stringsMx);
            s_cameraPref        = cam;
            s_lockInMessage     = lockMsg;
            s_labelSquats       = lSq;
            s_labelPushups      = lPu;
            s_labelSitups       = lSu;
            s_labelEarned       = lErn;
            s_labelReady        = lRdy;
            s_labelOutsideHours = lOut;
        }

        LogF(L"config: loaded from %s -> pushup=%d s, squat=%d s, situp=%d s, "
             L"min_unlock=%d s, camera='%s' lock_screen_only=%d",
             path.c_str(),
             s_pushupSec.load(), s_squatSec.load(), s_situpSec.load(), s_minUnlockSec.load(),
             cam.c_str(), s_cameraLockScreenOnly.load() ? 1 : 0);
        LogF(L"config: squat stand=%.1f down=%.1f gd=%.1f gu=%.1f "
             L"rep_ms=[%d..%d]",
             s_squatStandingAng.load(), s_squatDownAng.load(),
             s_squatGoingDownAng.load(), s_squatGoingUpAng.load(),
             s_squatMinRepMs.load(), s_squatMaxRepMs.load());
        LogF(L"config: pushup up=%.1f down=%.1f gd=%.1f gu=%.1f "
             L"plank>=%.1f rep_ms=[%d..%d]",
             s_pushupUpAng.load(), s_pushupDownAng.load(),
             s_pushupGoingDownAng.load(), s_pushupGoingUpAng.load(),
             s_pushupPlankMinAng.load(),
             s_pushupMinRepMs.load(), s_pushupMaxRepMs.load());
        LogF(L"config: situp lying=%.1f rising=%.1f lowering=%.1f sitting=%.1f rep_ms=[%d..%d]",
             s_situpLyingAng.load(), s_situpRisingAng.load(),
             s_situpLoweringAng.load(), s_situpSittingAng.load(),
             s_situpMinRepMs.load(), s_situpMaxRepMs.load());
        if (phone[0].have || phone[1].have || phone[2].have)
            LogF(L"config: phone calibration squat=%d pushup=%d situp=%d hipDrop=%.2f",
                 phone[0].have ? 1 : 0, phone[1].have ? 1 : 0, phone[2].have ? 1 : 0, phoneHipDrop);
        Rules(L"");   // logs the common rules
    }
}
