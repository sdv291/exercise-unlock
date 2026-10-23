// Log sink for the calibration tool. PoseEstimator.cpp (shared with the
// service) logs through Svc::LogF, which calls these; here they append to
// <DataDir>\calibrate.log instead of the service's own service.log.

#include "Log.h"
#include "Paths.h"

#include <mutex>
#include <string>

namespace ExerciseUnlock::Svc
{
    static std::mutex g_mx;

    void LogWriteToFile(const wchar_t* text)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        EnsureDataDir();
        HANDLE f = CreateFileW(DataPath(L"calibrate.log").c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t line[700]{};
        _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u  %s\r\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     text ? text : L"");
        const int need = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        if (need > 1)
        {
            std::string utf8(static_cast<size_t>(need - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), need - 1, nullptr, nullptr);
            DWORD wrote = 0;
            WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote, nullptr);
        }
        CloseHandle(f);
    }

    void LogWriteToTraceFile(const wchar_t*) {}
    void LogResetFiles() {}
}
