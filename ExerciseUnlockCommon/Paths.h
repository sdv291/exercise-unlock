#pragma once

#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace ExerciseUnlock
{
    // Folder for everything Exercise Unlock writes or reads at runtime:
    // vault.dat, state.dat, activity.log, service.log, users.allow,
    // sounds.ini + sounds\, provider log, webcam dumps.
    //
    //   * Installed build: <InstallDir>\data, InstallDir taken from
    //     HKLM\SOFTWARE\ExerciseUnlock (written by setup). Setup locks
    //     the folder to SYSTEM + Administrators — the vault is DPAPI
    //     machine-scope, so it must never be readable by the child.
    //   * Dev deployment (redeploy.ps1, no InstallDir value): the
    //     legacy %ProgramData%\ExerciseUnlock.
    //
    // Resolved once per process; every component (service, provider
    // in LogonUI, admin tool) uses the same rule, so they always agree.
    inline const std::wstring& DataDir()
    {
        static const std::wstring dir = []
        {
            wchar_t buf[MAX_PATH]{};
            DWORD cb = sizeof(buf);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ExerciseUnlock", L"InstallDir",
                             RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &cb) == ERROR_SUCCESS &&
                buf[0] != L'\0')
            {
                std::wstring d = buf;
                while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
                return d + L"\\data";
            }
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr,
                                           SHGFP_TYPE_CURRENT, buf)))
            {
                return std::wstring(buf) + L"\\ExerciseUnlock";
            }
            return std::wstring(L"C:\\ProgramData\\ExerciseUnlock");
        }();
        return dir;
    }

    inline std::wstring DataPath(const wchar_t* name)
    {
        return DataDir() + L"\\" + name;
    }

    // Create DataDir() if missing, with a protected DACL (SYSTEM +
    // Administrators, inherited by everything inside) so it never picks
    // up Program Files' "Users: read" from its parent.
    inline void EnsureDataDir()
    {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr))
        {
            SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
            CreateDirectoryW(DataDir().c_str(), &sa);
            LocalFree(sd);
        }
    }
}
