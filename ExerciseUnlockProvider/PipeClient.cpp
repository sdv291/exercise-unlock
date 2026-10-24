#include "PipeClient.h"
#include "Log.h"

#include <windows.h>
#include <sstream>
#include <iomanip>

using namespace ExerciseUnlock::Ipc;

namespace ExerciseUnlock::Client
{
    // The pipe name is public. Before believing "enough reps - start the
    // session", make sure the other end is the service (SYSTEM), or an
    // elevated developer build - not a look-alike server some other
    // process put up under the same name.
    static bool ServerIsTrusted(HANDLE pipe)
    {
        ULONG pid = 0;
        if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
        bool trusted = false, queried = false;
        if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            HANDLE token = nullptr;
            if (OpenProcessToken(proc, TOKEN_QUERY, &token))
            {
                DWORD user[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD) + 1]{};
                DWORD len = 0;
                if (GetTokenInformation(token, TokenUser, user, sizeof(user), &len))
                {
                    queried = true;
                    trusted = IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, WinLocalSystemSid) != FALSE;
                }
                TOKEN_ELEVATION elevation{};
                if (queried && !trusted && GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &len))
                    trusted = elevation.TokenIsElevated != 0;
                CloseHandle(token);
            }
            CloseHandle(proc);
        }
        if (!queried)
        {
            // LogonUI runs as SYSTEM and can read the token of any normal
            // process - the child's included. One it can't read is nothing
            // the child could start: accept it when it runs in session 0
            // (services) rather than lock the child out on a fluke.
            ULONG session = 1;
            trusted = GetNamedPipeServerSessionId(pipe, &session) && session == 0;
        }
        if (!trusted) LogF(L"pipe server (pid %lu) is not the service - ignored", pid);
        return trusted;
    }

    // Connects to the service's pipe in message mode, or fails.
    static HRESULT OpenServicePipe(unsigned timeoutMs, HANDLE& out)
    {
        out = INVALID_HANDLE_VALUE;
        // WaitNamedPipe first so we don't hang forever if the service
        // is down. If it's up, CreateFile connects immediately after.
        if (!WaitNamedPipeW(kPipeName, timeoutMs))
        {
            DWORD err = GetLastError();
            LogF(L"WaitNamedPipe timed out/failed: %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        HANDLE pipe = CreateFileW(
            kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            LogF(L"CreateFile(pipe) failed: %lu", err);
            return HRESULT_FROM_WIN32(err);
        }
        if (!ServerIsTrusted(pipe))
        {
            CloseHandle(pipe);
            return E_ACCESSDENIED;
        }

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
        out = pipe;
        return S_OK;
    }

    HRESULT QueryStatus(Response& out, unsigned timeoutMs)
    {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        const HRESULT hrOpen = OpenServicePipe(timeoutMs, pipe);
        if (FAILED(hrOpen)) return hrOpen;

        Request req{};
        req.magic   = kMagic;
        req.version = kVersion;
        req.op      = static_cast<uint16_t>(Op::GetStatus);

        DWORD written = 0;
        if (!WriteFile(pipe, &req, sizeof(req), &written, nullptr) || written != sizeof(req))
        {
            DWORD err = GetLastError();
            LogF(L"pipe WriteFile failed: %lu", err);
            CloseHandle(pipe);
            return HRESULT_FROM_WIN32(err);
        }

        Response resp{};
        DWORD read = 0;
        if (!ReadFile(pipe, &resp, sizeof(resp), &read, nullptr) || read != sizeof(resp))
        {
            DWORD err = GetLastError();
            LogF(L"pipe ReadFile failed: %lu (read=%lu)", err, read);
            CloseHandle(pipe);
            return HRESULT_FROM_WIN32(err);
        }

        CloseHandle(pipe);

        if (resp.magic != kMagic || resp.version != kVersion)
        {
            LogF(L"bad response header: magic=%08x ver=%u", resp.magic, resp.version);
            return E_FAIL;
        }

        out = resp;
        return S_OK;
    }

    std::wstring FormatStatus(const Response& r)
    {
        // Service pre-renders the whole label + counter + reward line
        // block into statusText using its Config-driven strings. We
        // just hand it to LogonUI.
        return std::wstring(r.statusText);
    }

    std::wstring FormatUnreachable()
    {
        return L"Exercise Unlock service is not running";
    }

    // Common helper: send a payload-less request with the given op,
    // drain the response, close.
    static HRESULT SendOpNoData(Op op, unsigned timeoutMs)
    {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        const HRESULT hrOpen = OpenServicePipe(timeoutMs, pipe);
        if (FAILED(hrOpen)) return hrOpen;

        Request req{};
        req.magic   = kMagic;
        req.version = kVersion;
        req.op      = static_cast<uint16_t>(op);

        DWORD written = 0;
        BOOL ok = WriteFile(pipe, &req, sizeof(req), &written, nullptr) &&
                  written == sizeof(req);
        if (ok)
        {
            Response resp{};
            DWORD read = 0;
            ReadFile(pipe, &resp, sizeof(resp), &read, nullptr);
        }
        CloseHandle(pipe);
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    HRESULT ResetCounts(unsigned timeoutMs)
    {
        return SendOpNoData(Op::ResetCounts, timeoutMs);
    }

    HRESULT StartSession(unsigned timeoutMs)
    {
        return SendOpNoData(Op::StartSession, timeoutMs);
    }
}
