#include "Log.h"

#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <mutex>
#include <string>
#include <cstdint>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace ExerciseUnlock::Svc
{
    static std::mutex g_logFileMx;
    static std::mutex g_traceFileMx;

    // Unmistakable canary — writes to a fixed world-writable path,
    // separate mutex, minimal code path, so we can see whether LogF is
    // even reaching us when service.log misbehaves.
    void LogWriteToTraceFile(const wchar_t* text)
    {
        std::lock_guard<std::mutex> lk(g_traceFileMx);
        HANDLE f = CreateFileW(
            L"C:\\Users\\Public\\eu-service-trace.log",
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t line[600]{};
        _snwprintf_s(line, _TRUNCATE,
                     L"%02u:%02u:%02u.%03u  %s\r\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     text ? text : L"(null)");
        int need = WideCharToMultiByte(CP_UTF8, 0, line, -1,
                                       nullptr, 0, nullptr, nullptr);
        if (need > 1)
        {
            std::string utf8(need - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, line, -1,
                                utf8.data(), need - 1, nullptr, nullptr);
            DWORD wrote = 0;
            WriteFile(f, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
        }
        FlushFileBuffers(f);
        CloseHandle(f);
    }

    static std::wstring LogFilePath()
    {
        wchar_t buf[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                       nullptr, SHGFP_TYPE_CURRENT, buf)))
        {
            return std::wstring(buf) + L"\\ExerciseUnlock\\service.log";
        }
        return L"C:\\ProgramData\\ExerciseUnlock\\service.log";
    }

    // SD granting SYSTEM + Admins full access. Applied when the log
    // file is first created; without it the file inherits whatever
    // the parent %ProgramData%\ExerciseUnlock happens to propagate,
    // which on some machines (a parent later granted OI/CI on their
    // own user) ends up NOT including SYSTEM — the service can create
    // the file the first time (parent ACL grants CreateFile) but
    // can't reopen it (own ACL doesn't grant SYSTEM append).
    static PSECURITY_DESCRIPTOR BuildLogSD()
    {
        PSECURITY_DESCRIPTOR sd = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;FA;;;SY)(A;;FA;;;BA)",
            SDDL_REVISION_1, &sd, nullptr);
        return sd;
    }

    // Open the log file for append. Share ALL modes so a curious
    // parent doing `Get-Content` (or Defender / Search Indexer) can't
    // lock us out. Retries a few times on sharing violation, and
    // self-heals a stale sdv29-only-ACL file left over from an
    // earlier build by deleting + recreating with an explicit SD.
    static HANDLE OpenLogForAppend(const std::wstring& path, DWORD& lastErr)
    {
        lastErr = 0;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            SECURITY_ATTRIBUTES sa{};
            PSECURITY_DESCRIPTOR sd = BuildLogSD();
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
            sa.bInheritHandle = FALSE;

            HANDLE f = CreateFileW(
                path.c_str(),
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                sd ? &sa : nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            const DWORD err = (f == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
            if (sd) LocalFree(sd);
            if (f != INVALID_HANDLE_VALUE) return f;
            lastErr = err;

            if (err == ERROR_ACCESS_DENIED)
            {
                // Old file with a busted ACL — nuke it and recreate.
                // SYSTEM has SeTakeOwnershipPrivilege by default so
                // DeleteFile should work even if the file's DACL
                // excludes SYSTEM (the *parent* dir gates delete and
                // its DACL does grant SYSTEM full access).
                DeleteFileW(path.c_str());
                continue;
            }
            if (err != ERROR_SHARING_VIOLATION) break;
            Sleep(20);
        }
        return INVALID_HANDLE_VALUE;
    }

    // Rolling log; rotates once at ~5 MB so an all-day session's DBG
    // spam doesn't fill the disk. Same UTF-8-with-BOM format as
    // activity.log so Notepad opens it cleanly. If the primary path
    // is somehow unwritable (permissions, sharing violation held
    // through the retries) the line is dropped from the file but
    // OutputDebugString in LogF has already shipped it, so nothing is
    // completely lost.
    // Route service.log errors into eu-service-trace.log so we can
    // see why writes are being dropped without needing DebugView.
    static void TraceFmt(const wchar_t* fmt, ...)
    {
        wchar_t buf[512]{};
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
        va_end(args);
        LogWriteToTraceFile(buf);
    }

    void LogResetFiles()
    {
        // Mirror LogWriteToFile's prelude: ensure the ProgramData dir
        // exists (first-ever start) before trying to touch service.log.
        wchar_t root[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                       nullptr, SHGFP_TYPE_CURRENT, root)))
        {
            const std::wstring dir = std::wstring(root) + L"\\ExerciseUnlock";
            CreateDirectoryW(dir.c_str(), nullptr);
        }

        // service.log: delete + recreate with explicit SD so a stale
        // sdv29-only ACL from a previous build is replaced in one shot
        // (also drops the rotated .1 copy — a fresh start means fresh).
        {
            std::lock_guard<std::mutex> lk(g_logFileMx);
            const std::wstring path = LogFilePath();
            const std::wstring older = path + L".1";
            DeleteFileW(older.c_str());
            DeleteFileW(path.c_str());

            SECURITY_ATTRIBUTES sa{};
            PSECURITY_DESCRIPTOR sd = BuildLogSD();
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
            sa.bInheritHandle = FALSE;
            HANDLE f = CreateFileW(
                path.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                sd ? &sa : nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (sd) LocalFree(sd);
            if (f != INVALID_HANDLE_VALUE)
            {
                const uint8_t bom[] = { 0xEF, 0xBB, 0xBF };
                DWORD wrote = 0;
                WriteFile(f, bom, sizeof(bom), &wrote, nullptr);
                CloseHandle(f);
            }
        }

        // trace file: just truncate; it lives in C:\Users\Public and
        // doesn't need the hardened SD (world-writable by design).
        {
            std::lock_guard<std::mutex> lk(g_traceFileMx);
            HANDLE f = CreateFileW(
                L"C:\\Users\\Public\\eu-service-trace.log",
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        }
    }

    void LogWriteToFile(const wchar_t* text)
    {
        std::lock_guard<std::mutex> lk(g_logFileMx);

        wchar_t root[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                       nullptr, SHGFP_TYPE_CURRENT, root)))
        {
            const std::wstring dir = std::wstring(root) + L"\\ExerciseUnlock";
            if (!CreateDirectoryW(dir.c_str(), nullptr))
            {
                DWORD e = GetLastError();
                if (e != ERROR_ALREADY_EXISTS)
                    TraceFmt(L"[LWF] CreateDirectory('%s') err=%lu", dir.c_str(), e);
            }
        }
        else
        {
            TraceFmt(L"[LWF] SHGetFolderPathW failed err=%lu", GetLastError());
        }

        const std::wstring path = LogFilePath();
        DWORD lastErr = 0;
        HANDLE f = OpenLogForAppend(path, lastErr);
        if (f == INVALID_HANDLE_VALUE)
        {
            TraceFmt(L"[LWF] CreateFile('%s') err=%lu", path.c_str(), lastErr);
            return;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f, &size))
        {
            TraceFmt(L"[LWF] GetFileSizeEx err=%lu", GetLastError());
        }
        if (size.QuadPart > 5 * 1024 * 1024)
        {
            CloseHandle(f);
            const std::wstring older = path + L".1";
            DeleteFileW(older.c_str());
            MoveFileW(path.c_str(), older.c_str());
            f = OpenLogForAppend(path, lastErr);
            if (f == INVALID_HANDLE_VALUE)
            {
                TraceFmt(L"[LWF] reopen after rotate err=%lu", lastErr);
                return;
            }
            size.QuadPart = 0;
        }

        if (size.QuadPart == 0)
        {
            const uint8_t bom[] = { 0xEF, 0xBB, 0xBF };
            DWORD wrote = 0;
            if (!WriteFile(f, bom, sizeof(bom), &wrote, nullptr))
                TraceFmt(L"[LWF] WriteFile(BOM) err=%lu", GetLastError());
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t ts[40]{};
        _snwprintf_s(ts, _TRUNCATE,
                     L"%04u-%02u-%02uT%02u:%02u:%02u.%03u  ",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::wstring wide = std::wstring(ts) + text + L"\r\n";
        int need = WideCharToMultiByte(CP_UTF8, 0,
                                       wide.c_str(), static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
        if (need <= 0)
        {
            TraceFmt(L"[LWF] WideCharToMultiByte sizing err=%lu", GetLastError());
        }
        else
        {
            std::string utf8(need, '\0');
            WideCharToMultiByte(CP_UTF8, 0,
                                wide.c_str(), static_cast<int>(wide.size()),
                                utf8.data(), need, nullptr, nullptr);
            DWORD wrote = 0;
            if (!WriteFile(f, utf8.data(),
                           static_cast<DWORD>(utf8.size()), &wrote, nullptr))
                TraceFmt(L"[LWF] WriteFile(line) err=%lu size=%zu", GetLastError(), utf8.size());
            else if (wrote != utf8.size())
                TraceFmt(L"[LWF] short write wrote=%lu of %zu", wrote, utf8.size());
        }
        FlushFileBuffers(f);
        CloseHandle(f);
    }
}
