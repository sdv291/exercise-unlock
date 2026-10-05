// Parent-run CLI to seed the credentials vault + harden the service.
//
//   ExerciseUnlockAdmin set          # prompts for domain / user / password
//   ExerciseUnlockAdmin set --user U --password P [--domain D]
//   ExerciseUnlockAdmin show         # prints stored user (never the password)
//   ExerciseUnlockAdmin clear        # deletes the vault
//   ExerciseUnlockAdmin test         # decrypts the vault and prints "OK <user>"
//   ExerciseUnlockAdmin harden       # locks down the service DACL so
//                                    # non-admin (child) accounts can't
//                                    # stop / pause / delete it
//
// Must be run elevated (writes to C:\ProgramData\ExerciseUnlock with
// an ACL that only SYSTEM / Administrators can access; SCM DACLs need
// SC_MANAGER_CONNECT + write-owner on the service).

#include "Vault.h"

#include <windows.h>
#include <sddl.h>
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

static bool ArgMatches(const wchar_t* arg, const wchar_t* a, const wchar_t* b = nullptr)
{
    return _wcsicmp(arg, a) == 0 || (b && _wcsicmp(arg, b) == 0);
}

static void PrintUsage()
{
    fwprintf(stdout,
        L"ExerciseUnlockAdmin — vault + service manager for Exercise Unlock.\n"
        L"\n"
        L"Commands:\n"
        L"  set                       prompt for domain/user/password\n"
        L"  set --user U --password P [--domain D]\n"
        L"  list                      print every user in the vault (never passwords)\n"
        L"  remove --user U           delete one user's entry from the vault\n"
        L"  test [--user U]           decrypt + print stored user (not password)\n"
        L"  clear                     delete the vault entirely\n"
        L"  harden                    lock service DACL so non-admins can't stop it\n"
        L"  grant --minutes N         hand the child N minutes of unlocked time\n"
        L"                            without requiring reps (parent override)\n"
        L"\n"
        L"Vault path: %ls\n"
        L"Run elevated. Domain '.' or empty means the local machine.\n",
        Vault::VaultPath().c_str());
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
            fwprintf(stderr, L"passwords do not match — vault unchanged\n");
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
        fwprintf(stderr, L"Store failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }
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
    fwprintf(stdout, L"granted %d minute(s) — session armed via pipe\n", minutes);
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
    //   (A;;CCLCSWRPWPDTLOCRRC;;;SY)  full to SYSTEM
    //   (A;;CCLCSWRPWPDTLOCRRC;;;BA)  full to Builtin\Administrators
    //   (A;;CCLCSWLOCRRC;;;IU)        query-only to Interactive Users
    //   (A;;CCLCSWLOCRRC;;;SU)        query-only to Service accounts
    //   (A;;CCLCSWLOCRRC;;;AU)        query-only to Authenticated Users
    // Access-mask abbreviations from
    //   https://learn.microsoft.com/windows/win32/services/service-security-and-access-rights
    // CC=QueryConfig, LC=QueryStatus, SW=EnumerateDependents,
    // RP=Start, WP=Stop, DT=PauseContinue, LO=Interrogate,
    // CR=UserDefinedControl, RC=ReadControl.
    // Restricted-users mask drops RP/WP/DT so they can't Start/Stop/Pause.
    static const wchar_t* kSddl =
        L"D:"
        L"(A;;CCLCSWRPWPDTLOCRRC;;;SY)"
        L"(A;;CCLCSWRPWPDTLOCRRC;;;BA)"
        L"(A;;CCLCSWLOCRRC;;;IU)"
        L"(A;;CCLCSWLOCRRC;;;SU)"
        L"(A;;CCLCSWLOCRRC;;;AU)";

    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm)
    {
        fwprintf(stderr, L"OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }
    SC_HANDLE hSvc = OpenServiceW(hScm, L"ExerciseUnlockService",
                                  WRITE_DAC | READ_CONTROL);
    if (!hSvc)
    {
        fwprintf(stderr, L"OpenService failed: %lu (is the service installed?)\n",
                 GetLastError());
        CloseServiceHandle(hScm);
        return 1;
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
        fwprintf(stderr, L"SetServiceObjectSecurity failed: %lu\n", err);
        return 1;
    }
    fwprintf(stdout,
        L"service DACL set. Only SYSTEM and Administrators can stop it.\n"
        L"Verify from the child's account: 'sc stop ExerciseUnlockService'\n"
        L"should return Access is denied.\n");
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
    std::wstring optDomain, optPass;
    for (int i = 2; i < argc - 1; ++i) {
        if      (ArgMatches(argv[i], L"--user"))     optUser    = argv[++i];
        else if (ArgMatches(argv[i], L"--domain"))   optDomain  = argv[++i];
        else if (ArgMatches(argv[i], L"--password")) optPass    = argv[++i];
        else if (ArgMatches(argv[i], L"--minutes"))  optMinutes = _wtoi(argv[++i]);
    }

    if (ArgMatches(argv[1], L"show", L"list"))   return CmdList();
    if (ArgMatches(argv[1], L"test"))            return CmdTest(optUser);
    if (ArgMatches(argv[1], L"clear"))           return CmdClear();
    if (ArgMatches(argv[1], L"harden"))          return CmdHarden();
    if (ArgMatches(argv[1], L"remove"))          return CmdRemove(optUser);
    if (ArgMatches(argv[1], L"grant"))           return CmdGrant(optMinutes);
    if (ArgMatches(argv[1], L"set"))             return CmdSetInteractive(optDomain, optUser, optPass);

    fwprintf(stderr, L"unknown command: %ls\n", argv[1]);
    PrintUsage();
    return 1;
}
