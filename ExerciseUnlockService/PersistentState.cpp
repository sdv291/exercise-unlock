#include "PersistentState.h"
#include "State.h"
#include "Log.h"
#include "Paths.h"

#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <mutex>
#include <chrono>
#include <cwchar>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace ExerciseUnlock::Svc
{
    // v6 (1.5.0, several accounts): a header - the active account and its
    // running session - then one record per account. Older files (one
    // set of counters) are not read: 1.5.0 is installed from scratch.
    #pragma pack(push, 1)
    struct FileHeader
    {
        uint32_t magic;                  // 'EUST'
        uint16_t version;                // 6
        uint16_t count;                  // records that follow
        int64_t  installStartUnix;
        int64_t  sessionExpiresAtUnix;   // the active account's session, 0 = none
        uint8_t  sessionIsGrace;
        uint8_t  reserved[7];
        wchar_t  active[Ipc::kUserChars];
    };
    struct FileRecord
    {
        wchar_t  name[Ipc::kUserChars];
        uint32_t squats, pushups, situps;
        uint32_t bankSeconds, pausedSeconds;
        int64_t  lastSessionEndedUnix;
        int32_t  dayKey;
    };
    #pragma pack(pop)

    static constexpr uint32_t kMagic     = 0x54535545;  // 'EUST' little-endian
    static constexpr uint16_t kVersion   = 6;
    static constexpr uint16_t kMaxUsers  = 64;

    static std::mutex g_ioMx;

    static std::wstring ProgramDataDir()
    {
        return DataDir();   // see Paths.h
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

        FileHeader head{};
        DWORD read = 0;
        BOOL ok = ReadFile(f, &head, sizeof(head), &read, nullptr);
        if (!ok || read != sizeof(head) || head.magic != kMagic || head.version != kVersion ||
            head.count > kMaxUsers)
        {
            CloseHandle(f);
            LogF(L"persistent: not a v%u state file (read=%lu magic=0x%08X v%u) - starting clean",
                 kVersion, read, head.magic, head.version);
            return;
        }
        std::vector<UserEntry> users;
        for (uint16_t i = 0; i < head.count; ++i)
        {
            FileRecord r{};
            if (!ReadFile(f, &r, sizeof(r), &read, nullptr) || read != sizeof(r)) break;
            r.name[Ipc::kUserChars - 1] = L'\0';
            UserEntry u;
            u.name = r.name;
            u.record.squats = r.squats;
            u.record.pushups = r.pushups;
            u.record.situps = r.situps;
            u.record.bankSeconds = r.bankSeconds;
            u.record.pausedSeconds = r.pausedSeconds;
            u.record.lastSessionEndedUnix = r.lastSessionEndedUnix;
            u.record.dayKey = r.dayKey;
            users.push_back(std::move(u));
        }
        CloseHandle(f);

        head.active[Ipc::kUserChars - 1] = L'\0';
        const std::wstring active = head.active;
        const size_t count = users.size();
        RestoreUsers(active, std::move(users));

        auto& st = GetState();
        st.installStartUnix.store(head.installStartUnix);
        // Restore the session only if it hasn't already expired —
        // otherwise a stale value would grant free unlock.
        using namespace std::chrono;
        const int64_t nowUnix = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count();
        if (head.sessionExpiresAtUnix > nowUnix)
        {
            st.sessionExpiresAtUnix.store(head.sessionExpiresAtUnix);
            st.sessionIsGrace.store(head.sessionIsGrace != 0);
        }
        LogF(L"persistent: loaded %zu account(s), active '%s': pushups=%u squats=%u situps=%u "
             L"bank=%us paused=%us session=%lld install=%lld",
             count, active.c_str(), st.pushUpCount.load(), st.squatCount.load(), st.sitUpCount.load(),
             st.earnedBankSeconds.load(), st.pausedSessionSeconds.load(),
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

        std::wstring active;
        std::vector<UserEntry> users = SnapshotUsers(active);
        if (users.size() > kMaxUsers) users.resize(kMaxUsers);

        std::vector<uint8_t> data(sizeof(FileHeader) + users.size() * sizeof(FileRecord));
        auto* head = reinterpret_cast<FileHeader*>(data.data());
        head->magic                = kMagic;
        head->version              = kVersion;
        head->count                = static_cast<uint16_t>(users.size());
        head->installStartUnix     = st.installStartUnix.load();
        head->sessionExpiresAtUnix = st.sessionExpiresAtUnix.load();
        head->sessionIsGrace       = st.sessionIsGrace.load() ? 1 : 0;
        wcsncpy_s(head->active, active.c_str(), _TRUNCATE);
        auto* rec = reinterpret_cast<FileRecord*>(data.data() + sizeof(FileHeader));
        for (size_t i = 0; i < users.size(); ++i)
        {
            const UserRecord& u = users[i].record;
            wcsncpy_s(rec[i].name, users[i].name.c_str(), _TRUNCATE);
            rec[i].squats  = u.squats;
            rec[i].pushups = u.pushups;
            rec[i].situps  = u.situps;
            rec[i].bankSeconds   = u.bankSeconds;
            rec[i].pausedSeconds = u.pausedSeconds;
            rec[i].lastSessionEndedUnix = u.lastSessionEndedUnix;
            rec[i].dayKey  = u.dayKey;
        }

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
        WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr);
        CloseHandle(f);
        if (sd) LocalFree(sd);
    }
}
