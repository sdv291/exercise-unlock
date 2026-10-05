#include "ActivityLog.h"
#include "Log.h"

#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <cwchar>
#include <chrono>
#include <mutex>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace ExerciseUnlock::Svc
{
    static std::mutex g_logMx;

    static std::wstring ProgramDataDir()
    {
        wchar_t buf[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                       nullptr, SHGFP_TYPE_CURRENT, buf)))
        {
            return std::wstring(buf) + L"\\ExerciseUnlock";
        }
        return L"C:\\ProgramData\\ExerciseUnlock";
    }

    static std::wstring LogPath()
    {
        return ProgramDataDir() + L"\\activity.log";
    }

    static HRESULT EnsureDir()
    {
        const std::wstring dir = ProgramDataDir();
        BOOL ok = CreateDirectoryW(dir.c_str(), nullptr);
        if (!ok && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }

    std::wstring ActivityLog::TimestampNow()
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t buf[32]{};
        _snwprintf_s(buf, _TRUNCATE,
                     L"%04u-%02u-%02uT%02u:%02u:%02u",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    void ActivityLog::Append(const std::wstring& line)
    {
        std::lock_guard<std::mutex> lk(g_logMx);
        if (FAILED(EnsureDir())) return;

        HANDLE f = CreateFileW(LogPath().c_str(),
                               FILE_APPEND_DATA, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            LogF(L"activity: CreateFile failed %lu", GetLastError());
            return;
        }

        // UTF-8 on disk — much easier for parents to open in Notepad
        // than a UTF-16 file. Add a BOM on first write so Notepad opens
        // it as UTF-8 without guessing.
        LARGE_INTEGER size{};
        GetFileSizeEx(f, &size);
        if (size.QuadPart == 0)
        {
            const uint8_t bom[] = { 0xEF, 0xBB, 0xBF };
            DWORD wrote = 0;
            WriteFile(f, bom, sizeof(bom), &wrote, nullptr);
        }

        std::wstring wide = TimestampNow() + L"  " + line + L"\r\n";
        int need = WideCharToMultiByte(CP_UTF8, 0,
                                       wide.c_str(), static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
        std::string utf8(need, '\0');
        WideCharToMultiByte(CP_UTF8, 0,
                            wide.c_str(), static_cast<int>(wide.size()),
                            utf8.data(), need, nullptr, nullptr);

        DWORD wrote = 0;
        WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
        CloseHandle(f);
    }

    void ActivityLog::ServiceStarted()
    {
        Append(L"service started");
    }

    void ActivityLog::ServiceStopping()
    {
        Append(L"service stopping");
    }

    void ActivityLog::UnlockGranted(uint32_t pushups,
                                    uint32_t squats,
                                    uint32_t earnedSeconds)
    {
        wchar_t line[128]{};
        _snwprintf_s(line, _TRUNCATE,
                     L"unlock granted: pushups=%u squats=%u earned=%us (%u:%02u)",
                     pushups, squats, earnedSeconds,
                     earnedSeconds / 60, earnedSeconds % 60);
        Append(line);
    }

    void ActivityLog::SessionExpired(uint32_t plannedSeconds)
    {
        wchar_t line[128]{};
        _snwprintf_s(line, _TRUNCATE,
                     L"session expired -> auto-lock (had %u:%02u)",
                     plannedSeconds / 60, plannedSeconds % 60);
        Append(line);
    }

    void ActivityLog::Note(const std::wstring& text)
    {
        Append(text);
    }
}
