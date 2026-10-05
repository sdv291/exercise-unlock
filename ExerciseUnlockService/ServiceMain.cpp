#include "ServiceMain.h"
#include "PipeServer.h"
#include "WebcamProbe.h"
#include "PoseEstimator.h"
#include "SessionManager.h"
#include "ActivityLog.h"
#include "PersistentState.h"
#include "UpdateChecker.h"
#include "Config.h"
#include "State.h"
#include "Log.h"
#include "Protocol.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

#include <shlwapi.h>
#include <wtsapi32.h>
#include <string>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace ExerciseUnlock::Svc
{
    static SERVICE_STATUS        g_status{};
    static SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
    static HANDLE                g_stopEvent    = nullptr;
    static PipeServer            g_server;
    static WebcamProbe           g_webcam;
    static PoseEstimator         g_pose;
    static SessionManager        g_session;
    static UpdateChecker         g_updater;

    SessionManager& GetSessionManager() { return g_session; }

    // Look for movenet_lightning.onnx next to the service exe. That's
    // where PostBuildEvent / redeploy.ps1 drop it. Absolute path so ORT
    // has nothing to guess at.
    static std::wstring FindModelPath(const wchar_t* filename)
    {
        wchar_t exePath[MAX_PATH]{};
        DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return {};
        PathRemoveFileSpecW(exePath);
        std::wstring p = exePath;
        p += L"\\";
        p += filename;
        return p;
    }

    static void InitPose()
    {
        auto& st = GetState();
        std::wstring model = FindModelPath(L"movenet_lightning.onnx");
        if (model.empty() || !PathFileExistsW(model.c_str()))
        {
            LogF(L"pose: model not found at expected path: %s", model.c_str());
            st.poseState.store(static_cast<uint8_t>(Ipc::PoseState::NoModel));
            return;
        }
        LogF(L"pose: loading %s", model.c_str());
        st.poseState.store(static_cast<uint8_t>(Ipc::PoseState::Loading));

        HRESULT hr = g_pose.Load(model);
        if (FAILED(hr))
        {
            st.poseState.store(static_cast<uint8_t>(Ipc::PoseState::Failed));
            st.poseLastHresult.store(static_cast<uint32_t>(hr));
            return;
        }
        hr = g_pose.ProveInference();
        if (FAILED(hr))
        {
            st.poseState.store(static_cast<uint8_t>(Ipc::PoseState::Failed));
            st.poseLastHresult.store(static_cast<uint32_t>(hr));
            return;
        }
        st.poseState.store(static_cast<uint8_t>(Ipc::PoseState::Ok));
        st.poseLastHresult.store(0);
    }

    static void SetStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHintMs = 0)
    {
        g_status.dwCurrentState  = state;
        g_status.dwWin32ExitCode = exitCode;
        g_status.dwWaitHint      = waitHintMs;

        // Accept Stop + SessionChange (so we get WTS_SESSION_LOCK etc
        // and can fold current rep counters into the earned bank).
        g_status.dwControlsAccepted =
            (state == SERVICE_START_PENDING) ? 0
                : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SESSIONCHANGE);

        static DWORD checkPoint = 1;
        g_status.dwCheckPoint =
            (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;

        if (g_statusHandle)
        {
            SetServiceStatus(g_statusHandle, &g_status);
        }
    }

    // Fold the current squat + pushup counters into earnedBankSeconds
    // using the same Config formula the reward path uses, then zero
    // the counters. Called on every WTS_SESSION_LOCK so a workout is
    // preserved even if the child steps away without clicking Submit.
    // Fold current rep counters into earnedBankSeconds. Uses the
    // same value SnapshotInto computes so the bank matches what the
    // tile displayed a moment ago (progression scale, if any, has
    // already been applied to per-rep seconds via the snapshot).
    // Add an "allow SYSTEM: Generic Read" ACE to the file / dir
    // at `path` (if it exists), merging with the current DACL via
    // SetEntriesInAcl rather than replacing it. `inherit` controls
    // whether sub-items (for a directory) also inherit the ACE.
    //
    // Returns silently on any missing file / API failure — this is
    // best-effort "please stop breaking". Logs so we can tell
    // what happened without a debugger attached.
    static void AddSystemReadAcl(const std::wstring& path, bool inherit)
    {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return;

        PACL  oldDacl = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        DWORD err = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                          DACL_SECURITY_INFORMATION,
                                          nullptr, nullptr, &oldDacl, nullptr, &sd);
        if (err != ERROR_SUCCESS)
        {
            LogF(L"acl: GetNamedSecurityInfo('%s') err=%lu", path.c_str(), err);
            return;
        }

        SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
        PSID systemSid = nullptr;
        if (!AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID,
                                      0, 0, 0, 0, 0, 0, 0, &systemSid))
        {
            LogF(L"acl: AllocateAndInitializeSid err=%lu", GetLastError());
            LocalFree(sd);
            return;
        }

        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = GENERIC_READ;
        ea.grfAccessMode        = GRANT_ACCESS;
        ea.grfInheritance       = inherit
            ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)
            : NO_INHERITANCE;
        ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType  = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(systemSid);

        PACL newDacl = nullptr;
        err = SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
        if (err == ERROR_SUCCESS)
        {
            err = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),
                                        SE_FILE_OBJECT,
                                        DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, newDacl, nullptr);
            if (err == ERROR_SUCCESS)
                LogF(L"acl: granted SYSTEM:(R) on '%s' (inherit=%d)",
                     path.c_str(), inherit ? 1 : 0);
            else
                LogF(L"acl: SetNamedSecurityInfo('%s') err=%lu",
                     path.c_str(), err);
        }
        else
        {
            LogF(L"acl: SetEntriesInAcl err=%lu", err);
        }

        if (newDacl)   LocalFree(newDacl);
        if (systemSid) FreeSid(systemSid);
        if (sd)        LocalFree(sd);
    }

    // Make sure the credential provider (which runs as SYSTEM in
    // LogonUI) can read its config files. See the call-site
    // comment in RunUntilStopped for the full rationale.
    static void EnsureSoundsAccessible()
    {
        wchar_t buf[MAX_PATH]{};
        if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                        nullptr, SHGFP_TYPE_CURRENT, buf)))
            return;
        const std::wstring base = std::wstring(buf) + L"\\ExerciseUnlock";
        AddSystemReadAcl(base + L"\\sounds.ini",   /*inherit=*/false);
        AddSystemReadAcl(base + L"\\sounds",       /*inherit=*/true);
        // users.allow restricts which local accounts get a tile.
        // Same SYSTEM-read trap as sounds.ini on first write.
        AddSystemReadAcl(base + L"\\users.allow",  /*inherit=*/false);
    }

    static void BankCurrentReps()
    {
        auto& st = GetState();
        const uint32_t pu = st.pushUpCount.load();
        const uint32_t sq = st.squatCount.load();
        if (pu == 0 && sq == 0) return;
        // Reuse SnapshotInto's math to keep the two paths consistent.
        Ipc::Response snap{};
        SnapshotInto(snap);
        const uint32_t existingBank = st.earnedBankSeconds.load();
        const uint32_t add = (snap.rewardEarnedSeconds > existingBank)
            ? (snap.rewardEarnedSeconds - existingBank) : 0;
        st.earnedBankSeconds.fetch_add(add);
        st.pushUpCount.store(0);
        st.squatCount.store(0);
        PersistentState::Save();
        LogF(L"bank: +%us (from %u pushups + %u squats) -> total %us",
             add, pu, sq, st.earnedBankSeconds.load());
    }

    static DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD eventType, LPVOID eventData, LPVOID)
    {
        switch (ctrl)
        {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            LogF(L"stop requested (ctrl=%lu)", ctrl);
            SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
            g_server.Stop();
            if (g_stopEvent) SetEvent(g_stopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_SESSIONCHANGE:
            // Workstation lock / unlock (and other session events).
            // On WTS_SESSION_LOCK we fold reps into the earned bank so
            // stepping away without clicking Submit doesn't lose the
            // workout. LOCK also fires when we ourselves triggered
            // the auto-lock after session-expiry — at that point
            // counters are already 0 so BankCurrentReps is a no-op.
            LogF(L"session change: eventType=%lu", eventType);
            if (eventType == WTS_SESSION_LOCK)
            {
                BankCurrentReps();
            }
            // Lock / unlock / logon / logoff / console switch can all
            // flip whether the camera should run (lock screen only) —
            // let the webcam thread re-check now, not at its next poll.
            g_webcam.Wake();
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            (void)eventData;
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
    }

    void RunUntilStopped(HANDLE stopEvent)
    {
        // Fresh log per service start — one run, one log, easier for
        // the parent to spot regressions without wading through yesterday.
        LogResetFiles();
        LogF(L"RunUntilStopped: enter (log reset at service start)");
        LogF(L"RunUntilStopped: -> Config::Load");
        Config::Load();             // one-shot at start; reward formula reads Config
        LogF(L"RunUntilStopped: <- Config::Load");
        LogF(L"RunUntilStopped: -> PersistentState::Load");
        PersistentState::Load();    // restore rep counters from last run
        LogF(L"RunUntilStopped: <- PersistentState::Load");
        LogF(L"RunUntilStopped: -> InitPose");
        InitPose();                 // one-shot at start; result lives in State
        LogF(L"RunUntilStopped: <- InitPose");
        LogF(L"RunUntilStopped: -> ActivityLog::ServiceStarted");
        ActivityLog::ServiceStarted();
        LogF(L"RunUntilStopped: <- ActivityLog::ServiceStarted");

        // Make sure sounds.ini and sounds\ (if present) are
        // readable by SYSTEM, which the LogonUI-hosted provider
        // runs as. ProgramData child files inherit from the parent
        // dir's DACL, and on at least one install the parent only
        // granted the admin user — a user-created sounds.ini then
        // couldn't be read by SYSTEM and all tile beeps fell back
        // to MessageBeep. One-shot icacls-style fix at service
        // start keeps that from biting other users.
        EnsureSoundsAccessible();
        g_webcam.Bind(&g_pose);     // feed every captured frame through pose
        LogF(L"RunUntilStopped: -> WebcamProbe::Start");
        g_webcam.Start(stopEvent);
        LogF(L"RunUntilStopped: -> SessionManager::Start");
        g_session.Start(stopEvent); // countdown + warnings + auto-lock
        LogF(L"RunUntilStopped: -> UpdateChecker::Start");
        g_updater.Start(stopEvent);
        LogF(L"RunUntilStopped: -> PipeServer::Run");
        g_server.Run(stopEvent);    // blocks until stopEvent fires
        LogF(L"RunUntilStopped: <- PipeServer::Run");
        g_updater.Join();
        g_session.Join();
        g_webcam.Join();
        PersistentState::Save();    // flush counters one last time before exit
        ActivityLog::ServiceStopping();
    }

    void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/)
    {
        g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_status.dwServiceSpecificExitCode = 0;

        g_statusHandle = RegisterServiceCtrlHandlerExW(
            kServiceName, ServiceCtrlHandler, nullptr);
        if (!g_statusHandle)
        {
            return;
        }

        SetStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_stopEvent)
        {
            SetStatus(SERVICE_STOPPED, GetLastError());
            return;
        }

        SetStatus(SERVICE_RUNNING);
        LogF(L"service running");

        RunUntilStopped(g_stopEvent);

        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;

        SetStatus(SERVICE_STOPPED);
    }
}
