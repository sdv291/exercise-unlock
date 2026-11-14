#include "State.h"
#include "Config.h"
#include "Log.h"
#include "PersistentState.h"

#include <chrono>
#include <cstring>
#include <cmath>
#include <cwchar>
#include <map>

namespace ExerciseUnlock::Svc
{
    static State g_state;

    State& GetState()
    {
        return g_state;
    }

    // ------------------------------------------------------------ users
    namespace
    {
        struct NoCase
        {
            bool operator()(const std::wstring& a, const std::wstring& b) const
            {
                return _wcsicmp(a.c_str(), b.c_str()) < 0;
            }
        };

        std::mutex g_usersMx;                                // the two below
        std::wstring g_active;                               // "" = nobody yet
        std::map<std::wstring, UserRecord, NoCase> g_users;  // all but the active one's live values

        UserRecord TakeLive()
        {
            UserRecord r;
            r.squats  = g_state.squatCount.load();
            r.pushups = g_state.pushUpCount.load();
            r.situps  = g_state.sitUpCount.load();
            r.bankSeconds   = g_state.earnedBankSeconds.load();
            r.pausedSeconds = g_state.pausedSessionSeconds.load();
            r.lastSessionEndedUnix = g_state.lastSessionEndedUnix.load();
            r.dayKey = g_state.dayKey.load();
            return r;
        }

        void PutLive(const UserRecord& r)
        {
            g_state.squatCount.store(r.squats);
            g_state.pushUpCount.store(r.pushups);
            g_state.sitUpCount.store(r.situps);
            g_state.earnedBankSeconds.store(r.bankSeconds);
            g_state.pausedSessionSeconds.store(r.pausedSeconds);
            g_state.lastSessionEndedUnix.store(r.lastSessionEndedUnix);
            g_state.dayKey.store(r.dayKey);
        }
    }

    std::wstring ActiveUser()
    {
        std::lock_guard<std::mutex> lk(g_usersMx);
        return g_active;
    }

    UserRules ActiveRules()
    {
        return Config::Rules(ActiveUser());
    }

    bool IsWithinHoursNow()
    {
        return ActiveRules().InWindowNow();
    }

    bool SelectUser(const std::wstring& user)
    {
        if (user.empty()) return false;
        std::lock_guard<std::mutex> lk(g_usersMx);
        if (_wcsicmp(user.c_str(), g_active.c_str()) == 0) return false;
        if (!g_active.empty()) g_users[g_active] = TakeLive();
        UserRecord next;
        if (auto it = g_users.find(user); it != g_users.end())
        {
            next = it->second;
            g_users.erase(it);
        }
        PutLive(next);
        LogF(L"user: '%s' -> '%s' (squats=%u pushups=%u situps=%u bank=%us paused=%us)",
             g_active.c_str(), user.c_str(), next.squats, next.pushups, next.situps,
             next.bankSeconds, next.pausedSeconds);
        g_active = user;
        return true;
    }

    std::vector<UserEntry> SnapshotUsers(std::wstring& active)
    {
        std::lock_guard<std::mutex> lk(g_usersMx);
        std::vector<UserEntry> out;
        active = g_active;
        if (!g_active.empty()) out.push_back({ g_active, TakeLive() });
        for (const auto& [name, record] : g_users) out.push_back({ name, record });
        return out;
    }

    void RestoreUsers(const std::wstring& active, std::vector<UserEntry> users)
    {
        std::lock_guard<std::mutex> lk(g_usersMx);
        g_users.clear();
        g_active.clear();
        PutLive(UserRecord{});
        for (UserEntry& u : users)
        {
            if (u.name.empty()) continue;
            if (!active.empty() && _wcsicmp(u.name.c_str(), active.c_str()) == 0)
            {
                g_active = u.name;
                PutLive(u.record);
            }
            else
                g_users[u.name] = u.record;
        }
        if (g_active.empty() && !active.empty()) g_active = active;   // no record yet: zeros
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
            LogF(L"day rollover %d -> %d: reset squats=%u pushups=%u situps=%u bank=%us paused=%us",
                 prev, today,
                 g_state.squatCount.load(), g_state.pushUpCount.load(), g_state.sitUpCount.load(),
                 g_state.earnedBankSeconds.load(), g_state.pausedSessionSeconds.load());
            g_state.squatCount.store(0);
            g_state.pushUpCount.store(0);
            g_state.sitUpCount.store(0);
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

        out.sitUpCount        = g_state.sitUpCount.load();
        out.sitUpHipAngle     = g_state.sitUpHipAngle.load();
        out.sitUpPhase        = g_state.sitUpPhase.load();
        out.reserved11[0] = out.reserved11[1] = out.reserved11[2] = 0;

        // The active account's rules: its minutes per rep, cap, hours.
        const UserRules rules = ActiveRules();

        // v1.1 progression: after `progression_after_days` from
        // install, scale per-rep reward down by
        //   1 / (1 + weeks_elapsed * factor)
        // where weeks_elapsed = (days_since_install - after_days) / 7.
        // Never below 0.25x so it stays reachable.
        auto progressionScale = [&rules]() -> float
        {
            const float factor    = rules.progressionFactor;
            const int   afterDays = rules.progressionDays;
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
        const uint32_t puSec = static_cast<uint32_t>(rules.pushupSec * scale);
        const uint32_t sqSec = static_cast<uint32_t>(rules.squatSec  * scale);
        const uint32_t suSec = static_cast<uint32_t>(rules.situpSec  * scale);
        const uint32_t paused = g_state.pausedSessionSeconds.load();
        uint32_t earned =
            g_state.earnedBankSeconds.load() +
            paused +
            out.pushUpCount * puSec +
            out.squatCount  * sqSec +
            out.sitUpCount  * suSec;
        // Hard cap: never hold more than max_session_seconds of
        // earned time. Reps above the cap still count on the tile
        // but add nothing.
        const int cap = rules.maxSessionSec;
        if (cap > 0 && earned > static_cast<uint32_t>(cap))
            earned = static_cast<uint32_t>(cap);
        // Weekend-aware min_unlock — same window logic as schedule.
        const uint32_t minUnlock = static_cast<uint32_t>(rules.MinUnlockToday());

        // Outside the schedule window we can't unlock at all, so force
        // rewardEnough to 0 regardless of what's in the bank. The bank
        // itself is preserved and shown so the child sees their credit
        // for tomorrow.
        const bool inWindow = rules.InWindowNow();
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
        const int     graceWin     = rules.graceWindowSec;
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
        //   <situps>: N
        //   <earned>: M:SS / M:SS [<ready>]
        const std::wstring lSq  = Config::LabelSquats();
        const std::wstring lPu  = Config::LabelPushups();
        const std::wstring lSu  = Config::LabelSitups();
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
        text += lSu;  text += L": "; text += std::to_wstring(out.sitUpCount);
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
            const int fromMin = rules.FromToday();
            const int toMin   = rules.ToToday();
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
