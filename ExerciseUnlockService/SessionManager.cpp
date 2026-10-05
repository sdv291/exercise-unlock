#include "SessionManager.h"
#include "State.h"
#include "Log.h"
#include "Config.h"
#include "ActivityLog.h"
#include "PersistentState.h"

#include <wtsapi32.h>
#include <userenv.h>
#include <chrono>
#include <ctime>
#include <string>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace ExerciseUnlock::Svc
{
    // Warning thresholds (seconds remaining). When the countdown crosses
    // one of these downwards, WTSSendMessage pops a modal message box in
    // the child's active session.
    static constexpr uint32_t kWarnAt[] = { 300 };   // 5 min only

    static int64_t NowUnix()
    {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    // 23:59:59 local time of the calendar day that contains `now`,
    // as a Unix timestamp. Used to cap an unlock session so it never
    // spills into tomorrow.
    static int64_t EndOfLocalDayUnix()
    {
        time_t nowT = time(nullptr);
        struct tm lt{};
        localtime_s(&lt, &nowT);
        lt.tm_hour = 23;
        lt.tm_min  = 59;
        lt.tm_sec  = 59;
        lt.tm_isdst = -1;   // let mktime figure out DST
        const time_t end = mktime(&lt);
        return static_cast<int64_t>(end);
    }

    // Runs a command line in the interactive console user's session,
    // detached. Used for LockWorkStation and for the PowerShell toast
    // one-liner. `cmdLine` is copied into a mutable buffer for the
    // CreateProcessAsUser lpCommandLine parameter, which needs write
    // access.
    static bool LaunchInConsoleSession(const std::wstring& cmdLine)
    {
        DWORD sessionId = WTSGetActiveConsoleSessionId();
        if (sessionId == 0xFFFFFFFF)
        {
            LogF(L"session: no active console");
            return false;
        }
        HANDLE hUser = nullptr;
        if (!WTSQueryUserToken(sessionId, &hUser))
        {
            LogF(L"session: WTSQueryUserToken failed %lu", GetLastError());
            return false;
        }
        HANDLE hPrimary = nullptr;
        if (!DuplicateTokenEx(hUser, TOKEN_ALL_ACCESS, nullptr,
                              SecurityImpersonation, TokenPrimary, &hPrimary))
        {
            LogF(L"session: DuplicateTokenEx failed %lu", GetLastError());
            CloseHandle(hUser);
            return false;
        }
        CloseHandle(hUser);

        LPVOID env = nullptr;
        if (!CreateEnvironmentBlock(&env, hPrimary, FALSE)) env = nullptr;

        std::wstring buf = cmdLine;   // CreateProcessAsUser mutates
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        PROCESS_INFORMATION pi{};

        BOOL ok = CreateProcessAsUserW(
            hPrimary, nullptr, buf.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
            env, nullptr, &si, &pi);
        if (ok)
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        else
        {
            LogF(L"session: CreateProcessAsUser failed %lu", GetLastError());
        }
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(hPrimary);
        return ok == TRUE;
    }

    // Non-blocking toast via PowerShell + WinForms NotifyIcon. Shows a
    // taskbar balloon in the child's session for ~8 seconds, then the
    // spawned PowerShell exits. Much better UX than WTSSendMessage's
    // modal MessageBox, which blocks whatever the child is doing.
    static void ShowUserToast(const std::wstring& title, const std::wstring& body)
    {
        // Escape single quotes inside title/body so the PS one-liner
        // doesn't get broken by a message that happens to contain '.
        auto esc = [](std::wstring s) {
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == L'\'') { s.insert(i, 1, L'\''); ++i; }
            }
            return s;
        };
        const std::wstring t = esc(title);
        const std::wstring b = esc(body);

        std::wstring ps;
        ps.reserve(600);
        ps += L"powershell.exe -NoProfile -WindowStyle Hidden -Command \"";
        ps += L"Add-Type -AssemblyName System.Windows.Forms;";
        ps += L"$n = New-Object System.Windows.Forms.NotifyIcon;";
        ps += L"$n.Icon = [System.Drawing.SystemIcons]::Warning;";
        ps += L"$n.BalloonTipTitle = '" + t + L"';";
        ps += L"$n.BalloonTipText = '"  + b + L"';";
        ps += L"$n.Visible = $true;";
        ps += L"$n.ShowBalloonTip(8000);";
        ps += L"Start-Sleep -Seconds 8;";
        ps += L"$n.Dispose()\"";

        bool shown = LaunchInConsoleSession(ps);
        if (!shown)
        {
            LogF(L"session: PS toast launch failed — using WTSSendMessage fallback");
        }
        // Belt-and-braces: ALSO raise a WTSSendMessage modal. The PS
        // toast depends on explorer.exe + NotifyIcon area + a working
        // audio stack — plenty to go wrong on a locked / fast-user-
        // switched session. WTSSendMessage goes through win32k.sys to
        // the destination session's winlogon desktop and reliably
        // raises a plain MessageBox even when nothing else works.
        // Non-blocking (dwTimeout=0, bWait=FALSE) so the countdown
        // thread isn't held up waiting for a Dismiss click.
        DWORD sess = WTSGetActiveConsoleSessionId();
        if (sess != 0xFFFFFFFF)
        {
            DWORD resp = 0;
            // Casts: WTSSendMessageW takes non-const LPWSTR; the
            // strings are owned by this scope so modifying them
            // would be fine but WTSSendMessage doesn't modify.
            BOOL ok = WTSSendMessageW(
                WTS_CURRENT_SERVER_HANDLE, sess,
                const_cast<LPWSTR>(title.c_str()),
                static_cast<DWORD>(title.size() * sizeof(wchar_t)),
                const_cast<LPWSTR>(body.c_str()),
                static_cast<DWORD>(body.size() * sizeof(wchar_t)),
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND,
                0, &resp, FALSE);
            if (!ok)
            {
                LogF(L"session: WTSSendMessage failed %lu", GetLastError());
            }
            else
            {
                LogF(L"session: WTSSendMessage shown in session %lu", sess);
            }
        }
    }

    // Launches "rundll32.exe user32.dll,LockWorkStation" as the console
    // user so LockWorkStation runs inside their interactive session
    // (SYSTEM can't call it directly and get the right result).
    static void LockConsoleSession()
    {
        LaunchInConsoleSession(L"rundll32.exe user32.dll,LockWorkStation");
    }

    void SessionManager::ArmSession(uint32_t seconds)
    {
        if (seconds == 0)
        {
            LogF(L"session: ArmSession(0) — ignored");
            return;
        }
        auto& st = GetState();
        const uint32_t pu = st.pushUpCount.load();
        const uint32_t sq = st.squatCount.load();

        // Hard cap: never grant more than Config::MaxSessionSeconds
        // in one unlock. Prevents a hoarded 20-pushup/50-squat
        // workout from unlocking the PC for the whole evening — the
        // child has to come back and re-earn after the cap.
        const uint32_t cap = Config::MaxSessionSeconds();
        if (cap > 0 && seconds > cap)
        {
            LogF(L"session: clamped %u -> %u (max_session_seconds)", seconds, cap);
            seconds = cap;
        }

        const int64_t now      = NowUnix();
        const int64_t requested = now + static_cast<int64_t>(seconds);
        const int64_t endOfDay = EndOfLocalDayUnix();
        int64_t expiresAt = requested;
        uint32_t effective = seconds;
        if (expiresAt > endOfDay)
        {
            expiresAt = endOfDay;
            effective = (expiresAt > now)
                ? static_cast<uint32_t>(expiresAt - now) : 0u;
            LogF(L"session: capped to end-of-day — %u sec (requested %u)",
                 effective, seconds);
        }
        if (effective == 0)
        {
            LogF(L"session: end-of-day already reached — not arming");
            return;
        }

        st.sessionExpiresAtUnix.store(expiresAt);
        // Persist so a crash / reboot doesn't grant untimed unlock.
        PersistentState::Save();
        LogF(L"session: armed for %u seconds", effective);
        ActivityLog::UnlockGranted(pu, sq, effective);
        if (m_wakeEvent) SetEvent(m_wakeEvent);
    }

    void SessionManager::Run()
    {
        LogF(L"session thread starting");
        auto& st = GetState();

        // Track which warnings we've already fired for the current
        // session so a warning only pops once even though the loop
        // ticks every second.
        int lastWarnIdx = -1;
        int64_t lastArmedExpiry = 0;

        while (WaitForSingleObject(m_stopEvent, 0) == WAIT_TIMEOUT)
        {
            const int64_t expiresAt = st.sessionExpiresAtUnix.load();

            if (expiresAt == 0)
            {
                lastWarnIdx = -1;
                lastArmedExpiry = 0;
                // Idle — wake on new session or stop.
                HANDLE waits[2] = { m_stopEvent, m_wakeEvent };
                WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                continue;
            }
            if (expiresAt != lastArmedExpiry)
            {
                lastWarnIdx     = -1;
                lastArmedExpiry = expiresAt;
            }

            const int64_t now = NowUnix();
            const int64_t remaining = expiresAt - now;

            if (remaining <= 0)
            {
                LogF(L"session: expired — locking workstation");
                st.sessionExpiresAtUnix.store(0);
                // Record that a session just ended — the credential
                // provider's grace-window check (close-your-apps) reads
                // this to decide whether to grant a short unlock
                // without the usual earned-time floor.
                st.lastSessionEndedUnix.store(NowUnix());
                st.squatCount.store(0);
                st.pushUpCount.store(0);
                PersistentState::Save();   // clears persisted session
                ActivityLog::SessionExpired(0);
                LockConsoleSession();
                lastWarnIdx     = -1;
                lastArmedExpiry = 0;
                continue;
            }

            // Fire the next warning if we've just crossed its threshold.
            for (int i = 0; i < static_cast<int>(std::size(kWarnAt)); ++i)
            {
                if (i <= lastWarnIdx) continue;
                if (static_cast<uint32_t>(remaining) <= kWarnAt[i])
                {
                    // Round UP to whole minutes so "just under 5 min"
                    // still reads as "5 minutes".
                    const uint32_t mins =
                        (static_cast<uint32_t>(remaining) + 59) / 60;
                    wchar_t minStr[16]{};
                    _snwprintf_s(minStr, _TRUNCATE, L"%u", mins);

                    // Load template and substitute {min}.
                    std::wstring body = Config::LockInMessage();
                    const std::wstring token = L"{min}";
                    for (;;)
                    {
                        const size_t at = body.find(token);
                        if (at == std::wstring::npos) break;
                        body.replace(at, token.size(), minStr);
                    }

                    LogF(L"session: warning (remaining=%llds) -> \"%ls\"",
                         static_cast<long long>(remaining), body.c_str());
                    ShowUserToast(L"Exercise Unlock", body);
                    lastWarnIdx = i;
                }
            }

            // Sleep 1 s (interruptible by stop / new arm).
            HANDLE waits[2] = { m_stopEvent, m_wakeEvent };
            WaitForMultipleObjects(2, waits, FALSE, 1000);
        }
        LogF(L"session thread stopping");
    }

    DWORD WINAPI SessionManager::Thunk(LPVOID param)
    {
        static_cast<SessionManager*>(param)->Run();
        return 0;
    }

    void SessionManager::Start(HANDLE stopEvent)
    {
        m_stopEvent = stopEvent;
        m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
        m_thread = CreateThread(nullptr, 0, Thunk, this, 0, nullptr);
    }

    void SessionManager::Join()
    {
        if (m_wakeEvent) SetEvent(m_wakeEvent);
        if (m_thread)
        {
            WaitForSingleObject(m_thread, INFINITE);
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
        if (m_wakeEvent)
        {
            CloseHandle(m_wakeEvent);
            m_wakeEvent = nullptr;
        }
    }
}
