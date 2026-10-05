#include "PipeServer.h"
#include "State.h"
#include "Log.h"
#include "Protocol.h"
#include "ServiceMain.h"
#include "SessionManager.h"
#include "PersistentState.h"
#include "Config.h"

#include <sddl.h>
#include <aclapi.h>
#include <algorithm>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>

using namespace ExerciseUnlock;
using namespace ExerciseUnlock::Ipc;

namespace ExerciseUnlock::Svc
{
    // ACL on the pipe. We leave owner/group unset — they default to the
    // creator, so any process can spin the service up (SCM -> SYSTEM,
    // manual run -> current admin). Setting O:SY explicitly needs
    // SeRestorePrivilege which normal admins don't have.
    //   D:P   = protected DACL (does not inherit)
    //     (A;;GA;;;SY) full access to SYSTEM (LogonUI, SCM)
    //     (A;;GA;;;BA) full access to Builtin\Administrators
    //     (A;;GRGW;;;AU) read+write to Authenticated Users (manual test)
    static constexpr wchar_t kSddl[] =
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)";

    struct SdHolder
    {
        PSECURITY_DESCRIPTOR sd{ nullptr };
        ~SdHolder() { if (sd) LocalFree(sd); }
    };

    static bool BuildPipeSecurity(SECURITY_ATTRIBUTES& sa, SdHolder& holder)
    {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kSddl, SDDL_REVISION_1, &holder.sd, nullptr))
        {
            LogF(L"ConvertStringSecurityDescriptor failed: %lu", GetLastError());
            return false;
        }
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        sa.lpSecurityDescriptor = holder.sd;
        return true;
    }

    // Fixed path we dump the latest webcam frame to on Op::SaveWebcamFrame.
    // C:\Users\Public\ inherits an ACL that grants Read to Everyone, so
    // an unelevated PowerShell running as the child (or the tester) can
    // pick the file up. The service runs as SYSTEM and can freely write
    // there.
    static constexpr wchar_t kWebcamDumpPath[] =
        L"C:\\Users\\Public\\ExerciseUnlockWebcam.bmp";

    // Dump one BGRA (top-down) frame from State to a 24-bit BMP file at
    // the fixed path above. Returns true on success. Called on
    // Op::SaveWebcamFrame; also mirrors the debug_frame.bmp writer in
    // WebcamProbe (kept there for one-shot startup diagnostics).
    static bool WriteLatestWebcamFrameBmp()
    {
        auto& st = GetState();
        std::vector<uint8_t> bgra;
        uint32_t w = 0, h = 0;
        {
            std::lock_guard<std::mutex> lk(st.latestFrameMx);
            if (st.latestFrameBgra.empty() ||
                st.latestFrameWidth  == 0 ||
                st.latestFrameHeight == 0)
            {
                return false;
            }
            bgra = st.latestFrameBgra;    // copy so we release the lock fast
            w = st.latestFrameWidth;
            h = st.latestFrameHeight;
        }

        HANDLE f = CreateFileW(kWebcamDumpPath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            LogF(L"SaveWebcamFrame: CreateFile('%s') failed %lu",
                 kWebcamDumpPath, GetLastError());
            return false;
        }
        #pragma pack(push, 1)
        struct { char m[2]; uint32_t size; uint32_t rsvd; uint32_t off;
                 uint32_t hdrSize; int32_t w; int32_t h;
                 uint16_t planes; uint16_t bpp;
                 uint32_t comp; uint32_t imgSize;
                 uint32_t xppm; uint32_t yppm;
                 uint32_t clrUsed; uint32_t clrImp; } bmp{};
        #pragma pack(pop)
        const uint32_t rowSize = ((w * 3 + 3) & ~3u);
        const uint32_t imgSize = rowSize * h;
        bmp.m[0] = 'B'; bmp.m[1] = 'M';
        bmp.size = 54 + imgSize;
        bmp.off  = 54;
        bmp.hdrSize = 40;
        bmp.w = static_cast<int32_t>(w);
        bmp.h = static_cast<int32_t>(h);    // positive = bottom-up
        bmp.planes = 1;
        bmp.bpp = 24;
        bmp.imgSize = imgSize;
        DWORD wrote = 0;
        WriteFile(f, &bmp, sizeof(bmp), &wrote, nullptr);

        std::vector<uint8_t> row(rowSize, 0);
        for (int y = static_cast<int>(h) - 1; y >= 0; --y)
        {
            const uint8_t* src = bgra.data() + static_cast<size_t>(y) * w * 4;
            for (uint32_t x = 0; x < w; ++x)
            {
                row[x * 3 + 0] = src[x * 4 + 0]; // B
                row[x * 3 + 1] = src[x * 4 + 1]; // G
                row[x * 3 + 2] = src[x * 4 + 2]; // R
            }
            WriteFile(f, row.data(), rowSize, &wrote, nullptr);
        }
        CloseHandle(f);
        LogF(L"SaveWebcamFrame: wrote %ux%u -> %s", w, h, kWebcamDumpPath);
        return true;
    }

    void PipeServer::Stop()
    {
        m_stop.store(true);
    }

    void PipeServer::Run(HANDLE stopEvent)
    {
        LogF(L"pipe server starting on %s", kPipeName);

        SECURITY_ATTRIBUTES sa{};
        SdHolder sdHolder;
        if (!BuildPipeSecurity(sa, sdHolder))
        {
            LogF(L"failed to build pipe SD — bailing out");
            return;
        }

        // Overlapped so ConnectNamedPipe can wait on either a client or
        // the stop event.
        while (!m_stop.load())
        {
            HANDLE pipe = CreateNamedPipeW(
                kPipeName,
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES,
                sizeof(Response),
                sizeof(Request),
                0,
                &sa);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                LogF(L"CreateNamedPipe failed: %lu", GetLastError());
                // Wait on the stop event instead of Sleep so Ctrl+C
                // (console mode) or SERVICE_CONTROL_STOP can break us
                // out immediately instead of after another 500 ms of
                // spamming the log.
                if (WaitForSingleObject(stopEvent, 500) == WAIT_OBJECT_0)
                {
                    break;
                }
                continue;
            }

            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent)
            {
                CloseHandle(pipe);
                continue;
            }

            BOOL connected = ConnectNamedPipe(pipe, &ov);
            DWORD err = GetLastError();
            bool ready = false;
            if (!connected && err == ERROR_IO_PENDING)
            {
                HANDLE waits[2] = { ov.hEvent, stopEvent };
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0)
                {
                    ready = true;
                }
                else
                {
                    // Stop requested. Cancel + close.
                    CancelIoEx(pipe, &ov);
                    CloseHandle(ov.hEvent);
                    CloseHandle(pipe);
                    break;
                }
            }
            else if (!connected && err == ERROR_PIPE_CONNECTED)
            {
                ready = true;
            }
            else if (!connected)
            {
                LogF(L"ConnectNamedPipe failed: %lu", err);
                CloseHandle(ov.hEvent);
                CloseHandle(pipe);
                continue;
            }

            CloseHandle(ov.hEvent);

            if (!ready)
            {
                CloseHandle(pipe);
                continue;
            }

            // Hand the connected pipe off to a worker thread. This
            // never blocks the accept loop, so we can serve concurrent
            // clients (LogonUI + admin app, say). We keep the thread
            // handle so we can wait on it during shutdown instead of
            // letting `sc stop` race with a hung ReadFile.
            auto* ctx = new ClientCtx{ this, pipe };
            HANDLE t = CreateThread(nullptr, 0, ClientThreadThunk, ctx, 0, nullptr);
            if (!t)
            {
                LogF(L"CreateThread failed: %lu", GetLastError());
                delete ctx;
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                continue;
            }
            TrackClientThread(t);
        }

        // Give outstanding clients a short window to finish their
        // single round trip. Anything past that is stuck (e.g. a
        // client that connected but never sent), so let the process
        // exit anyway — CRT tear-down will finalize them.
        JoinAllClientThreads(1000);

        LogF(L"pipe server stopped");
    }

    void PipeServer::TrackClientThread(HANDLE thread)
    {
        // Also opportunistically prune any that have already finished
        // so the vector doesn't grow without bound on a long-running
        // service.
        std::lock_guard<std::mutex> g(m_clientsMx);
        m_clientThreads.erase(
            std::remove_if(m_clientThreads.begin(), m_clientThreads.end(),
                [](HANDLE h)
                {
                    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
                    {
                        CloseHandle(h);
                        return true;
                    }
                    return false;
                }),
            m_clientThreads.end());
        m_clientThreads.push_back(thread);
    }

    void PipeServer::JoinAllClientThreads(DWORD perThreadTimeoutMs)
    {
        std::vector<HANDLE> local;
        {
            std::lock_guard<std::mutex> g(m_clientsMx);
            local.swap(m_clientThreads);
        }
        for (HANDLE h : local)
        {
            WaitForSingleObject(h, perThreadTimeoutMs);
            CloseHandle(h);
        }
    }

    DWORD WINAPI PipeServer::ClientThreadThunk(LPVOID param)
    {
        auto* ctx = static_cast<ClientCtx*>(param);
        ctx->self->HandleClient(ctx->pipe);
        delete ctx;
        return 0;
    }

    void PipeServer::HandleClient(HANDLE pipe)
    {
        // Read the fixed 8-byte header; leave any tail (op-specific
        // payload like Op::Grant's u32 minutes) for a follow-up read.
        Request req{};
        DWORD read = 0;
        if (!ReadFile(pipe, &req, sizeof(req), &read, nullptr) || read != sizeof(req))
        {
            LogF(L"ReadFile(client) failed: %lu (read=%lu)", GetLastError(), read);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }

        if (req.magic != kMagic || req.version != kVersion)
        {
            LogF(L"bad request: magic=%08x ver=%u", req.magic, req.version);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }

        Response resp{};
        switch (static_cast<Op>(req.op))
        {
        case Op::GetStatus:
            SnapshotInto(resp);
            break;

        case Op::ResetCounts:
        {
            // Zero the rep counters AND the bank. Kept around as an
            // explicit admin escape hatch; the actual "spend on
            // unlock" path lives in Op::StartSession below.
            auto& st = GetState();
            st.squatCount.store(0);
            st.pushUpCount.store(0);
            st.earnedBankSeconds.store(0);
            PersistentState::Save();
            LogF(L"counters + bank reset via pipe");
            SnapshotInto(resp);
            break;
        }

        case Op::SaveWebcamFrame:
        {
            // Dumps the most recently captured RGB32 frame to
            // kWebcamDumpPath. If the camera has never produced a frame
            // (e.g. session is active and camera released, or the
            // device just isn't there) the write is a no-op — a stale
            // file from a previous call is left alone rather than
            // truncated so the caller isn't confused by a 0-byte file.
            const bool ok = WriteLatestWebcamFrameBmp();
            if (!ok) LogF(L"SaveWebcamFrame: no frame available");
            SnapshotInto(resp);
            break;
        }

        case Op::Grant:
        {
            // Read the trailing 4-byte minutes payload. Anything less
            // is treated as bad request.
            uint32_t minutes = 0;
            DWORD rd = 0;
            if (!ReadFile(pipe, &minutes, sizeof(minutes), &rd, nullptr) ||
                rd != sizeof(minutes) || minutes == 0 || minutes > 12 * 60)
            {
                LogF(L"Grant: bad payload (rd=%lu min=%u)", rd, minutes);
                SnapshotInto(resp);
                break;
            }
            const uint32_t seconds = minutes * 60u;
            LogF(L"Grant: parent override — %u minutes", minutes);
            GetSessionManager().ArmSession(seconds);
            // Do NOT zero counters / bank — override is orthogonal
            // to earned time; the child keeps whatever they'd built.
            SnapshotInto(resp);
            break;
        }

        case Op::StartSession:
        {
            // Refuse to arm an unlock outside the schedule window.
            // Bank + counters are left intact so they carry over to
            // the next allowed period.
            if (!Config::IsWithinActiveWindowNow())
            {
                LogF(L"StartSession refused — outside schedule window");
                SnapshotInto(resp);
                break;
            }

            // Arm countdown for the currently-earned time, THEN zero
            // the earned bank AND the rep counters — everything the
            // child had is now spent on this unlock session.
            //
            // Grace path: if the child just finished a session and
            // comes back within Config::GraceWindowSeconds without
            // enough earned to cover min_unlock, grant a short
            // GraceUnlockSeconds unlock so they can close their
            // running apps without losing work. Bank / counters are
            // left alone — the grace unlock doesn't consume credit.
            Response tmp{};
            SnapshotInto(tmp);
            const int   minUnlock = Config::MinUnlockSecondsToday();
            const bool  earnedOk  = (tmp.rewardEarnedSeconds >= (uint32_t)minUnlock);
            uint32_t grantSec = tmp.rewardEarnedSeconds;
            bool graceGrant = false;

            auto& st = GetState();
            if (!earnedOk)
            {
                const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                const int64_t lastEnd = st.lastSessionEndedUnix.load();
                const int graceWin    = Config::GraceWindowSeconds();
                if (graceWin > 0 && lastEnd > 0 &&
                    (now - lastEnd) <= graceWin &&
                    st.sessionExpiresAtUnix.load() == 0)
                {
                    grantSec = (uint32_t)Config::GraceUnlockSeconds();
                    graceGrant = true;
                    LogF(L"StartSession: grace unlock %u sec (last ended %lld s ago)",
                         grantSec, (long long)(now - lastEnd));
                }
            }

            if (grantSec == 0)
            {
                SnapshotInto(resp);
                break;
            }

            LogF(L"StartSession requested %s=%u sec",
                 graceGrant ? L"grace" : L"earned", grantSec);
            GetSessionManager().ArmSession(grantSec);
            if (!graceGrant)
            {
                st.squatCount.store(0);
                st.pushUpCount.store(0);
                st.earnedBankSeconds.store(0);
            }
            // Clear the grace trigger so a single grace unlock can
            // only be redeemed once per expired session.
            st.lastSessionEndedUnix.store(0);
            PersistentState::Save();
            SnapshotInto(resp);
            break;
        }

        default:
            LogF(L"unknown op: %u", req.op);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }

        DWORD written = 0;
        if (!WriteFile(pipe, &resp, sizeof(resp), &written, nullptr) || written != sizeof(resp))
        {
            LogF(L"WriteFile(client) failed: %lu (written=%lu)", GetLastError(), written);
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}
