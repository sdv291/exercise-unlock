#include "PersistentState.h"
#include "State.h"
#include "Log.h"

#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <mutex>
#include <chrono>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace ExerciseUnlock::Svc
{
    // Layout evolution:
    //   v1 (16B): magic, version, reserved, pushupCount, squatCount
    //   v2 (20B): + earnedBankSeconds
    //   v3 (36B): + sessionExpiresAtUnix, installStartUnix
    #pragma pack(push, 1)
    struct FileLayout
    {
        uint32_t magic;              // 'EUST'
        uint16_t version;            // 3
        uint16_t reserved;
        uint32_t pushupCount;
        uint32_t squatCount;
        uint32_t earnedBankSeconds;  // v2+
        int64_t  sessionExpiresAtUnix; // v3+
        int64_t  installStartUnix;   // v3+
    };
    #pragma pack(pop)
    static_assert(sizeof(FileLayout) == 36, "unexpected layout");

    static constexpr uint32_t kMagic     = 0x54535545;  // 'EUST' little-endian
    static constexpr uint16_t kVersion   = 3;
    static constexpr size_t   kSizeV1    = 16;
    static constexpr size_t   kSizeV2    = 20;
    static constexpr size_t   kSizeV3    = 36;

    static std::mutex g_ioMx;

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

    static std::wstring StatePath()
    {
        return ProgramDataDir() + L"\\state.dat";
    }

    static HRESULT EnsureDirWithAcl()
    {
        const std::wstring dir = ProgramDataDir();
        // Same ACL as the Vault directory — SYSTEM + Admins only.
        const wchar_t* sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd, nullptr))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        BOOL ok = CreateDirectoryW(dir.c_str(), &sa);
        DWORD err = GetLastError();
        LocalFree(sd);
        if (!ok && err != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(err);
        return S_OK;
    }

    void PersistentState::Load()
    {
        std::lock_guard<std::mutex> lk(g_ioMx);
        HANDLE f = CreateFileW(StatePath().c_str(),
                               GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            LogF(L"persistent: no state file, starting clean");
            return;
        }

        FileLayout data{};
        DWORD read = 0;
        BOOL ok = ReadFile(f, &data, sizeof(data), &read, nullptr);
        CloseHandle(f);
        if (!ok || (read != kSizeV1 && read != kSizeV2 && read != kSizeV3))
        {
            LogF(L"persistent: short read (%lu) — ignoring", read);
            return;
        }
        if (data.magic != kMagic ||
            (data.version != 1 && data.version != 2 && data.version != kVersion))
        {
            LogF(L"persistent: bad magic/version (0x%08X v%u) — ignoring",
                 data.magic, data.version);
            return;
        }

        auto& st = GetState();
        st.pushUpCount.store(data.pushupCount);
        st.squatCount.store(data.squatCount);
        if (read >= kSizeV2) st.earnedBankSeconds.store(data.earnedBankSeconds);
        if (read >= kSizeV3)
        {
            // Restore session only if it hasn't already expired —
            // otherwise a stale value would grant free unlock.
            using namespace std::chrono;
            const int64_t nowUnix = duration_cast<seconds>(
                system_clock::now().time_since_epoch()).count();
            if (data.sessionExpiresAtUnix > nowUnix)
                st.sessionExpiresAtUnix.store(data.sessionExpiresAtUnix);
            st.installStartUnix.store(data.installStartUnix);
        }
        LogF(L"persistent: loaded pushups=%u squats=%u bank=%us "
             L"session=%lld install=%lld",
             data.pushupCount, data.squatCount,
             st.earnedBankSeconds.load(),
             (long long)st.sessionExpiresAtUnix.load(),
             (long long)st.installStartUnix.load());
    }

    void PersistentState::Save()
    {
        std::lock_guard<std::mutex> lk(g_ioMx);
        if (FAILED(EnsureDirWithAcl())) return;

        // Build explicit SD so the file is accessible to SYSTEM + Admins
        // regardless of what the parent directory's inheritable ACEs
        // propagate. (Observed 2026-10-01 on a machine where the parent
        // only had sdv29:(OI)(CI)(F) inheritable, so a SYSTEM-created
        // state.dat ended up without SYSTEM in its own DACL — every
        // subsequent CreateFile with GENERIC_WRITE failed err=5.)
        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;FA;;;SY)(A;;FA;;;BA)",
                SDDL_REVISION_1, &sd, nullptr))
        {
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = sd;
            sa.bInheritHandle = FALSE;
        }

        auto& st = GetState();

        // Stamp installStartUnix on first save if not already set.
        if (st.installStartUnix.load() == 0)
        {
            using namespace std::chrono;
            const int64_t nowUnix = duration_cast<seconds>(
                system_clock::now().time_since_epoch()).count();
            st.installStartUnix.store(nowUnix);
        }

        FileLayout data{};
        data.magic                = kMagic;
        data.version              = kVersion;
        data.pushupCount          = st.pushUpCount.load();
        data.squatCount           = st.squatCount.load();
        data.earnedBankSeconds    = st.earnedBankSeconds.load();
        data.sessionExpiresAtUnix = st.sessionExpiresAtUnix.load();
        data.installStartUnix     = st.installStartUnix.load();

        HANDLE f = CreateFileW(StatePath().c_str(),
                               GENERIC_WRITE, 0, sd ? &sa : nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD err0 = (f == INVALID_HANDLE_VALUE) ? GetLastError() : 0;

        // Self-heal: existing file with a stale/wrong ACL (sdv29-only
        // from a parent-dir inheritance quirk) will fail CreateFile
        // GENERIC_WRITE err=5 even for SYSTEM. Delete and retry.
        if (f == INVALID_HANDLE_VALUE && err0 == ERROR_ACCESS_DENIED)
        {
            DeleteFileW(StatePath().c_str());
            f = CreateFileW(StatePath().c_str(),
                            GENERIC_WRITE, 0, sd ? &sa : nullptr,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
                LogF(L"persistent: recreated state.dat after ACL-fix delete");
        }

        if (f == INVALID_HANDLE_VALUE)
        {
            LogF(L"persistent: CreateFile failed %lu (initial err=%lu)",
                 GetLastError(), err0);
            if (sd) LocalFree(sd);
            return;
        }
        DWORD wrote = 0;
        WriteFile(f, &data, sizeof(data), &wrote, nullptr);
        CloseHandle(f);
        if (sd) LocalFree(sd);
    }
}
