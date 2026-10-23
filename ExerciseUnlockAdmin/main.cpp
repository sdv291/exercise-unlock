// Parent-run CLI to seed the credentials vault + harden the service.
//
//   ExerciseUnlockAdmin set          # prompts for domain / user / password
//   ExerciseUnlockAdmin set --user U --password P [--domain D]
//   ExerciseUnlockAdmin set --user U --password-env VAR --domain D
//                                    # installer path: password comes from
//                                    # env var VAR (never on the command
//                                    # line / disk); VAR is cleared at once
//   ExerciseUnlockAdmin show        # prints stored user (never the password)
//   ExerciseUnlockAdmin clear        # deletes the vault
//   ExerciseUnlockAdmin test         # decrypts the vault and prints "OK <user>"
//   ExerciseUnlockAdmin verify --user U
//                                    # exit 0 if Windows still accepts the
//                                    # stored password (setup's update check)
//   ExerciseUnlockAdmin harden       # locks down the service DACL so
//                                    # non-admin (child) accounts can't
//                                    # stop / pause / delete it
//   ExerciseUnlockAdmin secure-data  # resets <DataDir> and everything in
//                                    # it to SYSTEM + Administrators only
//
// Must be run elevated (writes to <DataDir> with
// an ACL that only SYSTEM / Administrators can access; SCM DACLs need
// SC_MANAGER_CONNECT + write-owner on the service).

#include "Vault.h"
#include "Paths.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

using namespace ExerciseUnlock;

static std::wstring ReadLine()
{
    wchar_t buf[512]{};
    if (!fgetws(buf, static_cast<int>(std::size(buf)), stdin)) return {};
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s;
}

static std::wstring ReadPasswordHidden()
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode & ~(ENABLE_ECHO_INPUT));
    std::wstring s = ReadLine();
    SetConsoleMode(h, mode);
    fputws(L"\n", stdout);
    return s;
}

// Append one line to <DataDir>\admin.log. setup runs this tool hidden,
// so this is the only place its failures can be read afterwards.
static void AdminLog(const wchar_t* fmt, ...)
{
    wchar_t msg[600]{};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[700]{};
    _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    EnsureDataDir();
    HANDLE f = CreateFileW(DataPath(L"admin.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
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

static bool EnablePrivilege(const wchar_t* name)
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
                    AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) &&
                    GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

static bool ArgMatches(const wchar_t* arg, const wchar_t* a, const wchar_t* b = nullptr)
{
    return _wcsicmp(arg, a) == 0 || (b && _wcsicmp(arg, b) == 0);
}

static void PrintUsage()
{
    fwprintf(stdout,
        L"ExerciseUnlockAdmin - vault + service manager for Exercise Unlock.\n"
        L"\n"
        L"Commands:\n"
        L"  set                       prompt for domain/user/password\n"
        L"  set --user U --password P [--domain D]\n"
        L"  set --user U --password-env VAR --domain D\n"
        L"                            read the password from env var VAR (installer)\n"
        L"  list                     print every user in the vault (never passwords)\n"
        L"  remove --user U           delete one user's entry from the vault\n"
        L"  test [--user U]           decrypt + print stored user (not password)\n"
        L"  verify --user U           exit 0 if Windows still accepts U's stored password\n"
        L"  clear                     delete the vault entirely\n"
        L"  harden                    lock service DACL so non-admins can't stop it\n"
        L"  secure-data               reset data folder ACLs to SYSTEM + Administrators\n"
        L"  grant --minutes N         hand the child N minutes of unlocked time\n"
        L"                            without requiring reps (parent override)\n"
        L"\n"
        L"Vault path: %ls\n"
        L"Run elevated. Domain '.' or empty means the local machine.\n",
        Vault::VaultPath().c_str());
}

// Win32 code from an HRESULT as the exit code, so setup's messages say *why*.
static int ExitCodeOf(HRESULT hr)
{
    return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? static_cast<int>(HRESULT_CODE(hr)) : 1;
}

static int CmdSetInteractive(std::wstring domain, std::wstring user, std::wstring pass)
{
    if (domain.empty())
    {
        fwprintf(stdout, L"Domain (Enter for local machine): ");
        fflush(stdout);
        domain = ReadLine();
    }
    if (user.empty())
    {
        fwprintf(stdout, L"Username: ");
        fflush(stdout);
        user = ReadLine();
    }
    if (pass.empty())
    {
        fwprintf(stdout, L"Password: ");
        fflush(stdout);
        pass = ReadPasswordHidden();
        fwprintf(stdout, L"Confirm : ");
        fflush(stdout);
        const std::wstring confirm = ReadPasswordHidden();
        if (pass != confirm)
        {
            fwprintf(stderr, L"passwords do not match - vault unchanged\n");
            return 2;
        }
    }
    if (user.empty() || pass.empty())
    {
        fwprintf(stderr, L"user and password are required\n");
        return 2;
    }

    Vault::Credentials c{ domain, user, pass };
    HRESULT hr = Vault::Store(c);
    // Wipe our own copy right after Store returns — the encrypted
    // copy on disk is the only one we want to keep around.
    SecureZeroMemory(pass.data(), pass.size() * sizeof(wchar_t));
    if (FAILED(hr))
    {
        AdminLog(L"set: Store for '%ls' into %ls failed 0x%08X", user.c_str(),
                 Vault::VaultPath().c_str(), static_cast<unsigned>(hr));
        fwprintf(stderr, L"Store failed: 0x%08X\n", static_cast<unsigned>(hr));
        return ExitCodeOf(hr);
    }
    AdminLog(L"set: vault entry for '%ls' written", user.c_str());
    fwprintf(stdout, L"vault written to %ls\n", Vault::VaultPath().c_str());
    return 0;
}

static int CmdList()
{
    if (!Vault::Exists()) { fwprintf(stdout, L"no vault present\n"); return 0; }
    std::vector<Vault::Credentials> all;
    HRESULT hr = Vault::LoadAll(all);
    if (FAILED(hr)) {
        fwprintf(stderr, L"LoadAll failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }
    fwprintf(stdout, L"%zu entries in %ls:\n", all.size(), Vault::VaultPath().c_str());
    for (const auto& c : all) {
        fwprintf(stdout, L"  domain=%ls  user=%ls\n",
                 c.domain.empty() ? L"." : c.domain.c_str(),
                 c.username.c_str());
    }
    for (auto& c : all) SecureZeroMemory(c.password.data(), c.password.size() * sizeof(wchar_t));
    return 0;
}

static int CmdTest(const std::wstring& user)
{
    Vault::Credentials c;
    HRESULT hr = user.empty() ? Vault::Load(c) : Vault::Load(user, c);
    if (FAILED(hr)) {
        fwprintf(stderr, L"Load failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }
    fwprintf(stdout, L"OK  domain=%ls  user=%ls  (password %zu chars, hidden)\n",
             c.domain.empty() ? L"." : c.domain.c_str(),
             c.username.c_str(), c.password.size());
    SecureZeroMemory(c.password.data(), c.password.size() * sizeof(wchar_t));
    return 0;
}

// Setup's "keep the current settings?" check: does the vault hold a
// password for U that Windows still accepts? The password never leaves
// this process. Exit 0 = yes (also when Windows can't tell because
// network logons aren't granted to the account), 1326 = rejected or the
// account is gone, otherwise why the vault couldn't be read
// (1168 = no entry for U).
static int CmdVerify(const std::wstring& user)
{
    if (user.empty()) { fwprintf(stderr, L"verify requires --user U\n"); return 2; }
    // 1.1.0 could leave vault.dat with an empty ACL; with backup
    // privilege an administrator can still read it (Vault opens the
    // file with backup semantics).
    EnablePrivilege(SE_BACKUP_NAME);
    Vault::Credentials c;
    const HRESULT hr = Vault::Load(user, c);
    if (FAILED(hr))
    {
        AdminLog(L"verify: no readable vault entry for '%ls' (0x%08X)", user.c_str(), static_cast<unsigned>(hr));
        fwprintf(stderr, L"Load failed: 0x%08X\n", static_cast<unsigned>(hr));
        return ExitCodeOf(hr);
    }
    HANDLE token = nullptr;
    const BOOL ok = LogonUserW(c.username.c_str(), c.domain.empty() ? L"." : c.domain.c_str(),
                               c.password.c_str(), LOGON32_LOGON_NETWORK,
                               LOGON32_PROVIDER_DEFAULT, &token);
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    SecureZeroMemory(c.password.data(), c.password.size() * sizeof(wchar_t));
    if (token) CloseHandle(token);
    if (err != ERROR_SUCCESS && err != ERROR_LOGON_TYPE_NOT_GRANTED)
    {
        AdminLog(L"verify: Windows does not accept the stored password for '%ls' (%lu)", user.c_str(), err);
        fwprintf(stderr, L"stored password not accepted: %lu\n", err);
        return static_cast<int>(err);
    }
    AdminLog(L"verify: '%ls' ok", user.c_str());
    fwprintf(stdout, L"OK  user=%ls\n", c.username.c_str());
    return 0;
}

static int CmdRemove(const std::wstring& user)
{
    if (user.empty()) { fwprintf(stderr, L"remove requires --user U\n"); return 2; }
    HRESULT hr = Vault::Remove(user);
    if (FAILED(hr)) {
        fwprintf(stderr, L"Remove failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }
    fwprintf(stdout, L"user '%ls' removed (if it existed)\n", user.c_str());
    return 0;
}

static int CmdClear()
{
    HRESULT hr = Vault::Clear();
    if (FAILED(hr)) {
        fwprintf(stderr, L"Clear failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }
    fwprintf(stdout, L"vault cleared\n");
    return 0;
}

// --- Parent override: send an Op::Grant over the service pipe ---
static int CmdGrant(int minutes)
{
    if (minutes <= 0 || minutes > 12 * 60) {
        fwprintf(stderr, L"grant --minutes must be in 1..720\n"); return 2;
    }
    HANDLE h = CreateFileW(L"\\\\.\\pipe\\ExerciseUnlock",
                           GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"cannot open pipe (service running?): %lu\n", GetLastError());
        return 1;
    }
    // Request: magic 'EULK' + version=1 + op=5 (Grant) + minutes payload
    // Existing Request layout is 8 bytes, extending here: total 12 bytes.
    unsigned char req[12] = {
        0x45, 0x55, 0x4C, 0x4B,   // 'EULK'
        1, 0,                      // version
        5, 0,                      // Op::Grant = 5
        0, 0, 0, 0                 // minutes (u32 LE)
    };
    req[8]  = (unsigned char)(minutes         & 0xFF);
    req[9]  = (unsigned char)((minutes >> 8)  & 0xFF);
    req[10] = (unsigned char)((minutes >> 16) & 0xFF);
    req[11] = (unsigned char)((minutes >> 24) & 0xFF);
    DWORD wrote = 0;
    if (!WriteFile(h, req, sizeof(req), &wrote, nullptr) || wrote != sizeof(req)) {
        fwprintf(stderr, L"WriteFile failed: %lu\n", GetLastError());
        CloseHandle(h); return 1;
    }
    // Drain fixed 592-byte response.
    unsigned char resp[592];
    DWORD read = 0;
    ReadFile(h, resp, sizeof(resp), &read, nullptr);
    CloseHandle(h);
    fwprintf(stdout, L"granted %d minute(s) - session armed via pipe\n", minutes);
    return 0;
}

// Overwrite the SCM DACL on ExerciseUnlockService so:
//   * SYSTEM               — full control (needed for SCM to run it)
//   * Builtin\Administrators — full control (parent can manage it)
//   * everyone else        — only interrogate/query (can't stop/pause/delete/start)
// Without this the default SCM ACL usually lets standard users start /
// stop the service, which lets the child sidestep the whole system.
static int CmdHarden()
{
    // SDDL breakdown:
    //   D:            discretionary ACL
    //   (A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)  full to SYSTEM
    //   (A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)  full to Builtin\Administrators
    //   (A;;CCLCSWLOCRRC;;;IU)        query-only to Interactive Users
    //   (A;;CCLCSWLOCRRC;;;SU)        query-only to Service accounts
    //   (A;;CCLCSWLOCRRC;;;AU)        query-only to Authenticated Users
    // Access-mask abbreviations from
    //   https://learn.microsoft.com/windows/win32/services/service-security-and-access-rights
    // CC=QueryConfig, DC=ChangeConfig, LC=QueryStatus,
    // SW=EnumerateDependents, RP=Start, WP=Stop, DT=PauseContinue,
    // LO=Interrogate, CR=UserDefinedControl, SD=Delete, RC=ReadControl,
    // WD=WriteDac, WO=WriteOwner.
    // Restricted-users mask drops RP/WP/DT so they can't Start/Stop/Pause.
    // SYSTEM and Administrators keep full control (DC = change config,
    // SD = delete, WD/WO = change permissions): an earlier version left
    // them out, which locked setup itself out of reconfiguring, hardening
    // again and uninstalling the service.
    static const wchar_t* kSddl =
        L"D:"
        L"(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)"
        L"(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)"
        L"(A;;CCLCSWLOCRRC;;;IU)"
        L"(A;;CCLCSWLOCRRC;;;SU)"
        L"(A;;CCLCSWLOCRRC;;;AU)";

    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm)
    {
        const DWORD e = GetLastError();
        AdminLog(L"harden: OpenSCManager failed %lu", e);
        fwprintf(stderr, L"OpenSCManager failed: %lu\n", e);
        return static_cast<int>(e);
    }
    SC_HANDLE hSvc = OpenServiceW(hScm, L"ExerciseUnlockService",
                                  WRITE_DAC | READ_CONTROL);
    if (!hSvc && GetLastError() == ERROR_ACCESS_DENIED)
    {
        // Hardened by an older build: Administrators have no WRITE_DAC.
        // Take ownership (SeTakeOwnershipPrivilege) — the owner always
        // gets WRITE_DAC — then apply the corrected DACL below.
        EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
        if (SC_HANDLE own = OpenServiceW(hScm, L"ExerciseUnlockService", WRITE_OWNER))
        {
            PSECURITY_DESCRIPTOR osd = nullptr;
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BA", SDDL_REVISION_1, &osd, nullptr))
            {
                if (!SetServiceObjectSecurity(own, OWNER_SECURITY_INFORMATION, osd))
                    AdminLog(L"harden: take ownership failed %lu", GetLastError());
                LocalFree(osd);
            }
            CloseServiceHandle(own);
        }
        else
            AdminLog(L"harden: OpenService(WRITE_OWNER) failed %lu", GetLastError());
        hSvc = OpenServiceW(hScm, L"ExerciseUnlockService", WRITE_DAC | READ_CONTROL);
    }
    if (!hSvc)
    {
        const DWORD e = GetLastError();
        AdminLog(L"harden: OpenService failed %lu", e);
        fwprintf(stderr, L"OpenService failed: %lu (is the service installed?)\n", e);
        CloseServiceHandle(hScm);
        return static_cast<int>(e);
    }

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kSddl, SDDL_REVISION_1, &sd, nullptr))
    {
        fwprintf(stderr, L"parse SDDL failed: %lu\n", GetLastError());
        CloseServiceHandle(hSvc);
        CloseServiceHandle(hScm);
        return 1;
    }

    BOOL ok = SetServiceObjectSecurity(hSvc, DACL_SECURITY_INFORMATION, sd);
    DWORD err = GetLastError();
    LocalFree(sd);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hScm);

    if (!ok)
    {
        AdminLog(L"harden: SetServiceObjectSecurity failed %lu", err);
        fwprintf(stderr, L"SetServiceObjectSecurity failed: %lu\n", err);
        return static_cast<int>(err);
    }
    AdminLog(L"harden: ok");
    fwprintf(stdout,
        L"service DACL set. Only SYSTEM and Administrators can stop it.\n"
        L"Verify from the child's account: 'sc stop ExerciseUnlockService'\n"
        L"should return Access is denied.\n");
    return 0;
}

// Owner -> Administrators, DACL -> inherited ACEs only. Taking ownership
// first (SeTakeOwnershipPrivilege) brings back the owner's implicit
// WRITE_DAC on files whose DACL is empty.
static DWORD ResetToInherited(const std::wstring& path, PSID owner, PACL emptyAcl)
{
    const DWORD err = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                            OWNER_SECURITY_INFORMATION, owner, nullptr, nullptr, nullptr);
    if (err != ERROR_SUCCESS) return err;
    return SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                 nullptr, nullptr, emptyAcl, nullptr);
}

static void ResetTree(const std::wstring& dir, PSID owner, PACL emptyAcl, DWORD& firstErr)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        // SetNamedSecurityInfo would follow a junction / symlink out of
        // the data folder: leave those alone.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring path = dir + L"\\" + fd.cFileName;
        const DWORD err = ResetToInherited(path, owner, emptyAcl);
        if (err != ERROR_SUCCESS)
        {
            AdminLog(L"secure-data: %ls: error %lu", path.c_str(), err);
            if (firstErr == ERROR_SUCCESS) firstErr = err;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ResetTree(path, owner, emptyAcl, firstErr);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Resets <DataDir> to SYSTEM + Administrators full control (protected,
// inherited by everything inside) and every file / folder below it to
// inherited ACEs only, owned by Administrators.
//
// Setup used to do this with `icacls <dir> /inheritance:r /grant:r
// SID:(OI)(CI)F ... /T`, but /T runs that on every file too: a file
// can't take the (OI)(CI) grants, yet its inherited ACEs were still
// removed, leaving an *empty* DACL. After a reinstall nobody — not even
// setup — could rewrite vault.dat or users.allow, and LogonUI could no
// longer read them. This repairs those files as well.
static int CmdSecureData()
{
    EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);
    EnsureDataDir();
    const std::wstring root = DataDir();

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr))
    {
        const DWORD e = GetLastError();
        AdminLog(L"secure-data: parse SDDL failed %lu", e);
        return static_cast<int>(e);
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorOwner(sd, &owner, &defaulted);
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);

    ACL emptyAcl{};
    InitializeAcl(&emptyAcl, sizeof(emptyAcl), ACL_REVISION);

    // Children first, so that applying the folder's DACL last can
    // propagate into every one of them (they're all ours and unprotected
    // by then).
    DWORD firstErr = SetNamedSecurityInfoW(const_cast<LPWSTR>(root.c_str()), SE_FILE_OBJECT,
                                           OWNER_SECURITY_INFORMATION, owner, nullptr, nullptr, nullptr);
    if (firstErr != ERROR_SUCCESS)
        AdminLog(L"secure-data: %ls: owner error %lu", root.c_str(), firstErr);
    ResetTree(root, owner, &emptyAcl, firstErr);
    const DWORD err = SetNamedSecurityInfoW(const_cast<LPWSTR>(root.c_str()), SE_FILE_OBJECT,
                                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                            nullptr, nullptr, dacl, nullptr);
    LocalFree(sd);
    if (err != ERROR_SUCCESS)
    {
        AdminLog(L"secure-data: %ls: dacl error %lu", root.c_str(), err);
        firstErr = err;
    }

    if (firstErr != ERROR_SUCCESS)
    {
        fwprintf(stderr, L"secure-data: some entries could not be reset (first error %lu), see admin.log\n", firstErr);
        return static_cast<int>(firstErr);
    }
    AdminLog(L"secure-data: %ls reset to SYSTEM + Administrators", root.c_str());
    fwprintf(stdout, L"%ls reset to SYSTEM + Administrators only\n", root.c_str());
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2 || ArgMatches(argv[1], L"help", L"--help"))
    {
        PrintUsage();
        return argc < 2 ? 1 : 0;
    }

    // Parse common --user/--minutes flags once.
    std::wstring optUser;
    int optMinutes = 0;
    std::wstring optDomain, optPass, optPassEnv;
    for (int i = 2; i < argc - 1; ++i) {
        if      (ArgMatches(argv[i], L"--user"))         optUser    = argv[++i];
        else if (ArgMatches(argv[i], L"--domain"))       optDomain  = argv[++i];
        else if (ArgMatches(argv[i], L"--password"))     optPass    = argv[++i];
        else if (ArgMatches(argv[i], L"--password-env")) optPassEnv = argv[++i];
        else if (ArgMatches(argv[i], L"--minutes"))      optMinutes = _wtoi(argv[++i]);
    }

    // Installer path: the password travels in this process's environment
    // only — not on the command line (visible in the process list) and
    // not in a temp file. Clear the variable as soon as it's read.
    if (!optPassEnv.empty())
    {
        const DWORD need = GetEnvironmentVariableW(optPassEnv.c_str(), nullptr, 0);
        if (need > 1)
        {
            optPass.resize(need);
            const DWORD got = GetEnvironmentVariableW(optPassEnv.c_str(), optPass.data(), need);
            optPass.resize(got < need ? got : 0);
        }
        SetEnvironmentVariableW(optPassEnv.c_str(), nullptr);
        if (optPass.empty())
        {
            // Never fall through to the interactive prompt: the
            // installer runs us without a usable console.
            AdminLog(L"set: --password-env %ls is empty or missing", optPassEnv.c_str());
            fwprintf(stderr, L"--password-env %ls: variable is empty or missing\n", optPassEnv.c_str());
            return 2;
        }
    }

    if (ArgMatches(argv[1], L"show", L"list"))   return CmdList();
    if (ArgMatches(argv[1], L"test"))            return CmdTest(optUser);
    if (ArgMatches(argv[1], L"verify"))          return CmdVerify(optUser);
    if (ArgMatches(argv[1], L"clear"))           return CmdClear();
    if (ArgMatches(argv[1], L"harden"))          return CmdHarden();
    if (ArgMatches(argv[1], L"secure-data"))     return CmdSecureData();
    if (ArgMatches(argv[1], L"remove"))          return CmdRemove(optUser);
    if (ArgMatches(argv[1], L"grant"))           return CmdGrant(optMinutes);
    if (ArgMatches(argv[1], L"set"))             return CmdSetInteractive(optDomain, optUser, optPass);

    fwprintf(stderr, L"unknown command: %ls\n", argv[1]);
    PrintUsage();
    return 1;
}
