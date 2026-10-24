#include "State.h"
#include "Config.h"
#include "Log.h"
#include "PersistentState.h"

#include <chrono>
#include <cstring>
#include <cmath>

namespace ExerciseUnlock::Svc
{
    static State g_state;

    State& GetState()
    {
        return g_state;
    }

    bool IsSessionActive()
    {
        using namespace std::chrono;
        const int64_t now = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
        return g_state.sessionExpiresAtUnix.load() > now;
    }

    static int32_t TodayKey()
    {
        SYSTEMTIME lt{};
        GetLocalTime(&lt);
        return lt.wYear * 10000 + lt.wMonth * 100 + lt.wDay;
    }

    void RollOverDayIfNeeded()
    {
        const int32_t today = TodayKey();
        int32_t prev = g_state.dayKey.load();
        if (prev == today) return;
        if (!g_state.dayKey.compare_exchange_strong(prev, today)) return;  // another thread did it

        if (prev != 0)
        {
            LogF(L"day rollover %d -> %d: reset squats=%u pushups=%u bank=%us paused=%us",
                 prev, today,
                 g_state.squatCount.load(), g_state.pushUpCount.load(),
                 g_state.earnedBankSeconds.load(), g_state.pausedSessionSeconds.load());
            g_state.squatCount.store(0);
            g_state.pushUpCount.store(0);
            g_state.earnedBankSeconds.store(0);
            g_state.pausedSessionSeconds.store(0);
        }
        PersistentState::Save();
    }

    void SnapshotInto(Ipc::Response& out)
    {
        RollOverDayIfNeeded();

        out.magic   = Ipc::kMagic;
        out.version = Ipc::kVersion;
        out.op      = static_cast<uint16_t>(Ipc::Op::GetStatus);

        out.pushUpsDone = g_state.pushUpsDone.load();
        out.pushUpsGoal = g_state.pushUpsGoal.load();
        out.squatsDone  = g_state.squatsDone.load();
        out.squatsGoal  = g_state.squatsGoal.load();
        out.state       = g_state.challengeState.load();
        out.reserved[0] = out.reserved[1] = out.reserved[2] = 0;
        out.timeRemainingSeconds = g_state.timeRemainingSeconds.load();

        using namespace std::chrono;
        out.serverTimestampUnix = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();

        out.webcamState       = g_state.webcamState.load();
        out.reserved2[0] = out.reserved2[1] = out.reserved2[2] = 0;
        out.webcamFrames      = g_state.webcamFrames.load();
        out.webcamWidth       = g_state.webcamWidth.load();
        out.webcamHeight      = g_state.webcamHeight.load();
        out.webcamLastHresult = g_state.webcamLastHresult.load();

        out.poseState         = g_state.poseState.load();
        out.reserved3[0] = out.reserved3[1] = out.reserved3[2] = 0;
        out.poseLastHresult   = g_state.poseLastHresult.load();

        {
            std::lock_guard<std::mutex> lk(g_state.poseMx);
            std::memcpy(out.joints, g_state.joints, sizeof(out.joints));
        }
        out.poseAvgConfidence  = g_state.poseAvgConfidence.load();
        out.personDetected     = g_state.personDetected.load();
        out.reserved4[0] = out.reserved4[1] = out.reserved4[2] = 0;
        out.poseFramesInferred = g_state.poseFramesInferred.load();

        out.squatCount     = g_state.squatCount.load();
        out.squatKneeAngle = g_state.squatKneeAngle.load();
        out.squatPhase     = g_state.squatPhase.load();
        out.reserved5[0] = out.reserved5[1] = out.reserved5[2] = 0;

        out.pushUpCount       = g_state.pushUpCount.load();
        out.pushUpElbowAngle  = g_state.pushUpElbowAngle.load();
        out.pushUpPlankAngle  = g_state.pushUpPlankAngle.load();
        out.pushUpPhase       = g_state.pushUpPhase.load();
        out.reserved6[0] = out.reserved6[1] = out.reserved6[2] = 0;

        out.postureKind       = g_state.postureKind.load();
        out.reserved9[0] = out.reserved9[1] = out.reserved9[2] = 0;

        // v1.1 progression: after `progression_after_days` from
        // install, scale per-rep reward down by
        //   1 / (1 + weeks_elapsed * factor)
        // where weeks_elapsed = (days_since_install - after_days) / 7.
        // Never below 0.25x so it stays reachable.
        auto progressionScale = []() -> float
        {
            const float factor    = Config::ProgressionFactor();
            const int   afterDays = Config::ProgressionAfterDays();
            if (factor <= 0.0f || afterDays <= 0) return 1.0f;
            const int64_t start = g_state.installStartUnix.load();
            if (start == 0) return 1.0f;
            using namespace std::chrono;
            const int64_t now = duration_cast<seconds>(
                system_clock::now().time_since_epoch()).count();
            const float daysElapsed = (now - start) / 86400.0f;
            if (daysElapsed < afterDays) return 1.0f;
            const float weeks = (daysElapsed - afterDays) / 7.0f;
            const float scale = 1.0f / (1.0f + weeks * factor);
            return scale < 0.25f ? 0.25f : scale;
        };
        const float scale = progressionScale();
        const uint32_t puSec = static_cast<uint32_t>(Config::PushupSeconds() * scale);
        const uint32_t sqSec = static_cast<uint32_t>(Config::SquatSeconds()  * scale);
        const uint32_t paused = g_state.pausedSessionSeconds.load();
        uint32_t earned =
            g_state.earnedBankSeconds.load() +
            paused +
            out.pushUpCount * puSec +
            out.squatCount  * sqSec;
        // Hard cap: never hold more than max_session_seconds of
        // earned time. Reps above the cap still count on the tile
        // but add nothing.
        const int cap = Config::MaxSessionSeconds();
        if (cap > 0 && earned > static_cast<uint32_t>(cap))
            earned = static_cast<uint32_t>(cap);
        // Weekend-aware min_unlock — same window logic as schedule.
        const uint32_t minUnlock =
            static_cast<uint32_t>(Config::MinUnlockSecondsToday());

        // Outside the schedule window we can't unlock at all, so force
        // rewardEnough to 0 regardless of what's in the bank. The bank
        // itself is preserved and shown so the child sees their credit
        // for tomorrow.
        const bool inWindow = Config::IsWithinActiveWindowNow();
        out.rewardEarnedSeconds    = earned;
        out.rewardMinUnlockSeconds = minUnlock;

        // Grace window: the child's session just ended, give them a
        // short "close your apps" unlock without the earned-time
        // floor. Allowed past the schedule end too (session cut off
        // at 20:00) — it's one minute, once: lastSessionEndedUnix is
        // cleared on use and a grace session's expiry doesn't set it.
        const int64_t nowUnixGrace = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
        const int64_t lastEnd      = g_state.lastSessionEndedUnix.load();
        const int     graceWin     = Config::GraceWindowSeconds();
        const bool    inGrace      = (graceWin > 0 && lastEnd > 0 &&
                                      (nowUnixGrace - lastEnd) <= graceWin &&
                                      g_state.sessionExpiresAtUnix.load() == 0);

        // A paused session remainder unlocks even below min_unlock.
        out.rewardEnough           = (inWindow && (earned >= minUnlock || paused > 0)) ? 1 :
                                     (inGrace                                        ) ? 1 : 0;
        out.reserved7[0] = out.reserved7[1] = out.reserved7[2] = 0;

        const int64_t nowUnix = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
        const int64_t expiresAt = g_state.sessionExpiresAtUnix.load();
        if (expiresAt > nowUnix)
        {
            out.sessionActive           = 1;
            out.sessionRemainingSeconds = static_cast<uint32_t>(expiresAt - nowUnix);
        }
        else
        {
            out.sessionActive           = 0;
            out.sessionRemainingSeconds = 0;
        }
        out.reserved8[0] = out.reserved8[1] = out.reserved8[2] = 0;

        // Pre-render the tile text with Config labels so the provider
        // (LogonUI) doesn't need to know the label strings.
        //   <squats>: N
        //   <pushups>: N
        //   <earned>: M:SS / M:SS [<ready>]
        const std::wstring lSq  = Config::LabelSquats();
        const std::wstring lPu  = Config::LabelPushups();
        const std::wstring lErn = Config::LabelEarned();
        const std::wstring lRdy = Config::LabelReady();
        auto ms = [](uint32_t s) -> std::wstring
        {
            wchar_t buf[16]{};
            _snwprintf_s(buf, _TRUNCATE, L"%u:%02u", s / 60, s % 60);
            return buf;
        };
        std::wstring text;
        text.reserve(120);
        text += lSq;  text += L": "; text += std::to_wstring(out.squatCount);
        text += L"\r\n";
        text += lPu;  text += L": "; text += std::to_wstring(out.pushUpCount);
        text += L"\r\n";
        text += lErn; text += L": "; text += ms(out.rewardEarnedSeconds);
        text += L" / "; text += ms(out.rewardMinUnlockSeconds);
        if (out.rewardEnough)
        {
            text += L"  ";
            text += lRdy;
        }

        // v1.1 — surface an available update as its own line so a
        // parent / child sees it without opening the dashboard.
        {
            std::lock_guard<std::mutex> lk(g_state.updateMx);
            if (g_state.updateAvailable && !g_state.updateLatestVersion.empty())
            {
                text += L"\r\n⬆ Update to v";
                text += g_state.updateLatestVersion;
            }
        }

        // Show a schedule notice on its own line when the current time
        // is outside the allowed window. Reps aren't counted and the
        // provider will refuse StartSession, so surface why nothing is
        // happening instead of silently ignoring input.
        if (!inWindow)
        {
            const int fromMin = Config::ScheduleFromMinutesToday();
            const int toMin   = Config::ScheduleToMinutesToday();
            wchar_t fromBuf[8]{}, toBuf[8]{};
            _snwprintf_s(fromBuf, _TRUNCATE, L"%02d:%02d", fromMin / 60, fromMin % 60);
            _snwprintf_s(toBuf,   _TRUNCATE, L"%02d:%02d", toMin   / 60, toMin   % 60);
            std::wstring notice = Config::LabelOutsideHours();
            auto replace = [&](const wchar_t* tok, const wchar_t* val)
            {
                const std::wstring token = tok;
                for (;;)
                {
                    const size_t at = notice.find(token);
                    if (at == std::wstring::npos) break;
                    notice.replace(at, token.size(), val);
                }
            };
            replace(L"{from}", fromBuf);
            replace(L"{to}",   toBuf);
            text += L"\r\n";
            text += notice;
        }

        // Copy safely into fixed-size buffer.
        const size_t maxChars = std::size(out.statusText) - 1;
        const size_t n = text.size() < maxChars ? text.size() : maxChars;
        std::memcpy(out.statusText, text.data(), n * sizeof(wchar_t));
        out.statusText[n] = L'\0';

        out.phoneState = g_state.phoneState.load();
        out.offHoursReps = g_state.offHoursReps.load();
        out.reserved10[0] = out.reserved10[1] = 0;
        {
            std::lock_guard<std::mutex> lk(g_state.phoneMx);
            const size_t maxUrl = std::size(out.phoneUrl) - 1;
            const size_t u = g_state.phoneUrl.size() < maxUrl ? g_state.phoneUrl.size() : maxUrl;
            std::memcpy(out.phoneUrl, g_state.phoneUrl.data(), u * sizeof(wchar_t));
            out.phoneUrl[u] = L'\0';
        }
    }
}
