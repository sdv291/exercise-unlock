#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>

namespace ExerciseUnlock::Svc
{
    // Configuration read from config.ini next to the service exe.
    // Numeric values are cached in atomics; the camera preference is
    // a wstring guarded by its own mutex. Load() is called once at
    // service start; hot-reload comes later.
    class Config
    {
    public:
        static void Load();

        static int PushupSeconds()    { return s_pushupSec.load(); }
        static int SquatSeconds()     { return s_squatSec.load(); }
        static int MinUnlockSeconds() { return s_minUnlockSec.load(); }
        // Hard cap on a single unlock session (seconds). 0 = disabled.
        static int MaxSessionSeconds() { return s_maxSessionSec.load(); }
        // Grace window after a session ends: during this many seconds
        // the credential provider will grant a short "close-your-stuff"
        // unlock without the earned-time check. 0 = disabled.
        static int GraceWindowSeconds() { return s_graceWindowSec.load(); }
        // Duration of the grace unlock itself (seconds).
        static int GraceUnlockSeconds() { return s_graceUnlockSec.load(); }

        // Squat detector thresholds (knee angle, degrees) and cycle
        // timing (whole rep = start of descent -> back at standing).
        static float SquatStandingAngle()  { return s_squatStandingAng.load(); }
        static float SquatDownAngle()      { return s_squatDownAng.load(); }
        static float SquatGoingDownAngle() { return s_squatGoingDownAng.load(); }
        static float SquatGoingUpAngle()   { return s_squatGoingUpAng.load(); }
        static int   SquatMinRepMs()       { return s_squatMinRepMs.load(); }
        static int   SquatMaxRepMs()       { return s_squatMaxRepMs.load(); }

        // v1.1 alternative detector mode. 0 = angle (default,
        // current knee-angle thresholds), 1 = floor-relative hip
        // height (uses the hip-frac thresholds below), 2 = both
        // (require angle AND floor to agree).
        enum SquatMode { AngleMode = 0, FloorMode = 1, BothMode = 2 };
        static int   SquatDetectorMode()        { return s_squatMode.load(); }
        // Floor-mode thresholds. hipFrac = (ankleY - hipY)/(ankleY - shoulderY).
        // Standing ~0.45..0.55, deep squat near 0.
        static float SquatStandingHipFrac()     { return s_squatStandHipFrac.load(); }
        static float SquatDownHipFrac()         { return s_squatDownHipFrac.load(); }
        static float SquatGoingDownHipFrac()    { return s_squatGoDownHipFrac.load(); }
        static float SquatGoingUpHipFrac()      { return s_squatGoUpHipFrac.load(); }

        // Push-up detector thresholds (elbow angle, degrees), plank
        // gate (shoulder-hip-ankle) and cycle timing.
        static float PushupUpAngle()        { return s_pushupUpAng.load(); }
        static float PushupDownAngle()      { return s_pushupDownAng.load(); }
        static float PushupGoingDownAngle() { return s_pushupGoingDownAng.load(); }
        static float PushupGoingUpAngle()   { return s_pushupGoingUpAng.load(); }
        static float PushupPlankMinAngle()  { return s_pushupPlankMinAng.load(); }
        static int   PushupMinRepMs()       { return s_pushupMinRepMs.load(); }
        static int   PushupMaxRepMs()       { return s_pushupMaxRepMs.load(); }

        // Daily active window (minutes since local midnight). Outside
        // it, exercises don't count and StartSession is refused.
        // Weekday (Mon-Fri) and weekend (Sat-Sun) can have different
        // windows and different min-unlock times. Wrap-around
        // (from > to) is supported and treated as overnight.
        // Legacy [schedule]/active_from & active_to still work as a
        // fallback for both weekday and weekend.
        static int  ScheduleFromMinutesToday();
        static int  ScheduleToMinutesToday();
        static int  MinUnlockSecondsToday();
        static bool IsWithinActiveWindowNow();

        // Difficulty progression: after `progression_days` from first
        // install, every reward-per-rep is scaled by
        //   1 / (1 + weeks_elapsed * progression_factor).
        // 0 for either disables progression.
        static float ProgressionFactor()   { return s_progressionFactor.load(); }
        static int   ProgressionAfterDays(){ return s_progressionDays.load(); }

        // Form-quality gate: reject reps whose measured range doesn't
        // reach at least this fraction of the full angle span. 0 =
        // don't gate on quality. Also used as multiplier — a rep at
        // quality 0.8 counts as 0.8x its base reward.
        static float SquatQualityMin()  { return s_squatQualityMin.load(); }
        static float PushupQualityMin() { return s_pushupQualityMin.load(); }

        // Walking-vs-squatting gate: maximum |ΔhipX| allowed between
        // the start of descent and rep completion, in normalized frame
        // widths (0..1). A real squat keeps the hip essentially in
        // place (|Δ| < 0.05); walking across the frame drifts far
        // more. 0 disables the gate.
        static float SquatMaxHipDrift() { return s_squatMaxHipDrift.load(); }

        // Knee-lift-vs-squatting gate: minimum ΔhipY REQUIRED between
        // rep start and the deepest point in the rep, in normalized
        // frame heights (0..1). A real squat drops the hip 0.10..0.20;
        // a one-leg knee lift leaves it put (<0.03). 0 disables.
        static float SquatMinHipDrop() { return s_squatMinHipDrop.load(); }

        // Bypass detection: comma-separated substrings; any camera
        // whose friendly name contains one is refused. Case-
        // insensitive. Also the max consecutive identical frames the
        // liveness check tolerates before webcam is marked failed.
        static std::wstring VirtualCameraBlocklist();
        static int  LivenessMaxStaticFrames() { return s_livenessMaxStatic.load(); }

        // Sound feedback: if true the provider plays a short beep
        // when the pushUp/squat counter increments.
        static bool SoundOnRep() { return s_soundOnRep.load(); }

        // v1.1 update checker: HTTPS GET of a tiny version manifest
        // every N hours. 0 h disables the periodic check (but a
        // one-shot startup check still runs when enabled=1).
        static bool         UpdatesEnabled()        { return s_updateEnabled.load(); }
        static int          UpdateIntervalHours()   { return s_updateIntervalH.load(); }
        static std::wstring UpdateManifestUrl();

        static std::wstring CameraPreference();
        // true (default): the camera runs only while the console shows
        // the lock / sign-in screen. false: legacy always-on (except
        // during an unlock session) — handy for skeleton-viewer
        // calibration from the parent's desktop.
        static bool CameraLockScreenOnly() { return s_cameraLockScreenOnly.load(); }
        static std::wstring LockInMessage();
        static std::wstring LabelSquats();
        static std::wstring LabelPushups();
        static std::wstring LabelEarned();
        static std::wstring LabelReady();
        // Rendered on the tile when the current local time is outside
        // the schedule window. Placeholders {from} and {to} are replaced
        // with the configured HH:MM strings.
        static std::wstring LabelOutsideHours();

    private:
        static std::atomic<int> s_pushupSec;
        static std::atomic<int> s_squatSec;
        static std::atomic<int> s_minUnlockSec;
        static std::atomic<int> s_maxSessionSec;
        static std::atomic<int> s_graceWindowSec;
        static std::atomic<int> s_graceUnlockSec;

        static std::atomic<float> s_squatStandingAng;
        static std::atomic<float> s_squatDownAng;
        static std::atomic<float> s_squatGoingDownAng;
        static std::atomic<float> s_squatGoingUpAng;
        static std::atomic<int>   s_squatMinRepMs;
        static std::atomic<int>   s_squatMaxRepMs;

        static std::atomic<int>   s_squatMode;
        static std::atomic<float> s_squatStandHipFrac;
        static std::atomic<float> s_squatDownHipFrac;
        static std::atomic<float> s_squatGoDownHipFrac;
        static std::atomic<float> s_squatGoUpHipFrac;

        static std::atomic<float> s_pushupUpAng;
        static std::atomic<float> s_pushupDownAng;
        static std::atomic<float> s_pushupGoingDownAng;
        static std::atomic<float> s_pushupGoingUpAng;
        static std::atomic<float> s_pushupPlankMinAng;
        static std::atomic<int>   s_pushupMinRepMs;
        static std::atomic<int>   s_pushupMaxRepMs;

        static std::atomic<int>   s_activeFromMin;
        static std::atomic<int>   s_activeToMin;
        static std::atomic<int>   s_weekdayFromMin;
        static std::atomic<int>   s_weekdayToMin;
        static std::atomic<int>   s_weekendFromMin;
        static std::atomic<int>   s_weekendToMin;
        static std::atomic<int>   s_weekdayMinUnlockSec;
        static std::atomic<int>   s_weekendMinUnlockSec;

        static std::atomic<float> s_progressionFactor;
        static std::atomic<int>   s_progressionDays;

        static std::atomic<float> s_squatQualityMin;
        static std::atomic<float> s_pushupQualityMin;
        static std::atomic<float> s_squatMaxHipDrift;
        static std::atomic<float> s_squatMinHipDrop;

        static std::wstring       s_vcamBlocklist;
        static std::atomic<int>   s_livenessMaxStatic;

        static std::atomic<bool>  s_soundOnRep;

        static std::atomic<bool>  s_cameraLockScreenOnly;

        static std::atomic<bool>  s_updateEnabled;
        static std::atomic<int>   s_updateIntervalH;
        static std::wstring       s_updateUrl;

        static std::wstring       s_labelOutsideHours;
        static std::mutex       s_stringsMx;
        static std::wstring     s_cameraPref;
        static std::wstring     s_lockInMessage;
        static std::wstring     s_labelSquats;
        static std::wstring     s_labelPushups;
        static std::wstring     s_labelEarned;
        static std::wstring     s_labelReady;
    };
}
