#include "Config.h"
#include "Log.h"

#include <shlwapi.h>
#include <string>
#include <cwchar>
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

    // Default active window: 07:00..20:00 local time.
    std::atomic<int>   Config::s_activeFromMin      { 7 * 60 };
    std::atomic<int>   Config::s_activeToMin        { 20 * 60 };
    // Weekday / weekend overrides — INT_MIN sentinel = "not set,
    // fall through to legacy active_from/to and min_unlock_seconds".
    std::atomic<int>   Config::s_weekdayFromMin     { INT_MIN };
    std::atomic<int>   Config::s_weekdayToMin       { INT_MIN };
    std::atomic<int>   Config::s_weekendFromMin     { INT_MIN };
    std::atomic<int>   Config::s_weekendToMin       { INT_MIN };
    std::atomic<int>   Config::s_weekdayMinUnlockSec{ INT_MIN };
    std::atomic<int>   Config::s_weekendMinUnlockSec{ INT_MIN };

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

    // Camera only on the lock / sign-in screen by default.
    std::atomic<bool>  Config::s_cameraLockScreenOnly{ true };

    std::atomic<bool>  Config::s_updateEnabled      { false };
    std::atomic<int>   Config::s_updateIntervalH    { 24 };
    std::wstring       Config::s_updateUrl          { L"" };

    std::mutex       Config::s_stringsMx;
    std::wstring     Config::s_cameraPref;
    std::wstring     Config::s_lockInMessage{ L"Computer will lock in {min} minutes" };
    std::wstring     Config::s_labelSquats  { L"Squats" };
    std::wstring     Config::s_labelPushups { L"Push-ups" };
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

    int Config::ScheduleFromMinutesToday()
    {
        const int day = IsWeekendNow()
            ? s_weekendFromMin.load() : s_weekdayFromMin.load();
        return (day != INT_MIN) ? day : s_activeFromMin.load();
    }
    int Config::ScheduleToMinutesToday()
    {
        const int day = IsWeekendNow()
            ? s_weekendToMin.load() : s_weekdayToMin.load();
        return (day != INT_MIN) ? day : s_activeToMin.load();
    }
    int Config::MinUnlockSecondsToday()
    {
        const int day = IsWeekendNow()
            ? s_weekendMinUnlockSec.load() : s_weekdayMinUnlockSec.load();
        return (day != INT_MIN) ? day : s_minUnlockSec.load();
    }

    bool Config::IsWithinActiveWindowNow()
    {
        SYSTEMTIME lt{};
        GetLocalTime(&lt);
        const int nowMin = lt.wHour * 60 + lt.wMinute;
        const int from = ScheduleFromMinutesToday();
        const int to   = ScheduleToMinutesToday();
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

    static std::wstring FindConfigPath()
    {
        wchar_t exe[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
        PathRemoveFileSpecW(exe);
        std::wstring p = exe;
        p += L"\\config.ini";
        return p;
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

        // --- schedule (HH:MM strings) ---
        auto parseHHMM = [](const std::wstring& s, int dflt) -> int
        {
            if (s.empty()) return dflt;
            wchar_t* end = nullptr;
            long h = wcstol(s.c_str(), &end, 10);
            if (end == s.c_str() || *end != L':') return dflt;
            long m = wcstol(end + 1, nullptr, 10);
            if (h < 0 || h > 23 || m < 0 || m > 59) return dflt;
            return static_cast<int>(h) * 60 + static_cast<int>(m);
        };
        auto parseHHMMOptional = [&](const std::wstring& s) -> int
        {
            if (s.empty()) return INT_MIN;
            return parseHHMM(s, INT_MIN);
        };
        const std::wstring sFrom = readStr(L"schedule", L"active_from", L"07:00");
        const std::wstring sTo   = readStr(L"schedule", L"active_to",   L"20:00");
        s_activeFromMin.store(parseHHMM(sFrom, s_activeFromMin.load()));
        s_activeToMin.store  (parseHHMM(sTo,   s_activeToMin.load()));

        // Weekday / weekend windows (optional; empty = fall back to
        // active_from/to). Same syntax.
        s_weekdayFromMin.store(parseHHMMOptional(readStr(L"schedule", L"weekday_from", L"")));
        s_weekdayToMin.store  (parseHHMMOptional(readStr(L"schedule", L"weekday_to",   L"")));
        s_weekendFromMin.store(parseHHMMOptional(readStr(L"schedule", L"weekend_from", L"")));
        s_weekendToMin.store  (parseHHMMOptional(readStr(L"schedule", L"weekend_to",   L"")));

        auto readIntOpt = [&](const wchar_t* section, const wchar_t* key) -> int
        {
            const int probe = GetPrivateProfileIntW(section, key, -424242, path.c_str());
            return (probe == -424242) ? INT_MIN : probe;
        };
        s_weekdayMinUnlockSec.store(readIntOpt(L"schedule", L"weekday_min_unlock_seconds"));
        s_weekendMinUnlockSec.store(readIntOpt(L"schedule", L"weekend_min_unlock_seconds"));

        // Progression [reward] section, optional keys.
        s_progressionDays.store(GetPrivateProfileIntW(
            L"reward", L"progression_after_days", s_progressionDays.load(), path.c_str()));
        s_progressionFactor.store(readFloat(L"reward", L"progression_factor",
                                            s_progressionFactor.load()));

        // Quality gates.
        s_squatQualityMin.store (readFloat(L"squat",  L"quality_min", s_squatQualityMin.load()));
        s_pushupQualityMin.store(readFloat(L"pushup", L"quality_min", s_pushupQualityMin.load()));
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
        const std::wstring updUrl = readStr(L"update", L"manifest_url", L"");

        {
            std::lock_guard<std::mutex> lk(s_stringsMx);
            s_vcamBlocklist = vcam;
            s_updateUrl     = updUrl;
        }

        s_cameraLockScreenOnly.store(GetPrivateProfileIntW(
            L"camera", L"lock_screen_only",
            s_cameraLockScreenOnly.load() ? 1 : 0, path.c_str()) != 0);

        std::wstring cam     = readStr(L"camera",   L"preferred",       L"");
        std::wstring lockMsg = readStr(L"warnings", L"lock_in_message",
                                       L"Computer will lock in {min} minutes");
        std::wstring lSq  = readStr(L"labels", L"squats",  L"Squats");
        std::wstring lPu  = readStr(L"labels", L"pushups", L"Push-ups");
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
            s_labelEarned       = lErn;
            s_labelReady        = lRdy;
            s_labelOutsideHours = lOut;
        }

        LogF(L"config: loaded from %s -> pushup=%d s, squat=%d s, "
             L"min_unlock=%d s, camera='%s' lock_screen_only=%d",
             path.c_str(),
             s_pushupSec.load(), s_squatSec.load(), s_minUnlockSec.load(),
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
        LogF(L"config: schedule active %02d:%02d..%02d:%02d local",
             s_activeFromMin.load() / 60, s_activeFromMin.load() % 60,
             s_activeToMin.load()   / 60, s_activeToMin.load()   % 60);
    }
}
