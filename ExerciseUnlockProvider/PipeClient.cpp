#include "PipeClient.h"
#include "Log.h"

#include <windows.h>
#include <sstream>
#include <iomanip>

using namespace ExerciseUnlock::Ipc;

namespace ExerciseUnlock::Client
{
    HRESULT QueryStatus(Response& out, unsigned timeoutMs)
    {
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

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

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
        if (!WaitNamedPipeW(kPipeName, timeoutMs))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        HANDLE pipe = CreateFileW(kPipeName,
                                  GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

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
