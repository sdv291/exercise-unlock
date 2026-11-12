// ExerciseUnlockUpdate.exe — the "new version" window.
//
//   ExerciseUnlockUpdate.exe /version 1.3.5 /download <url> [/sha256 <hex>] [/notes <url>]
//                            [/change-en <text>]... [/change-uk <text>]... [/ask] [/lang en|uk]
//
// The service starts it in the signed-in user's session when its update
// check finds a newer version (UpdateChecker.cpp). An administrator gets
// "Download and install" and the /change-* points ("What's new"); with
// /ask - a standard account, the child - it says to ask a parent, with a
// small "Install now" link for a parent standing by. "Download and install"
// fetches the installer into %TEMP% with a progress bar, checks it against
// the SHA-256 from the server's version.txt and runs it elevated (Windows
// asks for an administrator) as a silent update: setup keeps the child's
// account, password and calibration. /FROMAPP=1 makes setup open its
// normal wizard instead when the stored password no longer works.
//
// Runs as the user, not elevated. Language: /lang, else the one setup ran
// in (HKLM\SOFTWARE\ExerciseUnlock\Language), else Windows'.

#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include "Version.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
    enum Txt
    {
        tAvailable, tInstalled, tInstall, tLater, tDownloading, tConnecting, tProgress, tProgressMb,
        tStarting, tFailed, tNetError, tHttpError, tWriteError, tBadHash, tCancelled, tRunError, tRetry, tCancel, tClose, tChanges, tAsk, tInstallNow, tGotIt,
        tCount
    };

    const wchar_t* const kEn[tCount] = {
        L"Exercise Unlock %s is available",
        L"Installed: %s. The update keeps the user's account, password and calibration.",
        L"Download and install\nWindows will ask for an administrator's permission.",
        L"Later",
        L"Downloading Exercise Unlock %s",
        L"Connecting…",
        L"%.1f of %.1f MB",
        L"%.1f MB",
        L"Waiting for an administrator's permission…",
        L"The update was not installed",
        L"Couldn't download the update (error %lu). Check the internet connection and try again.",
        L"Couldn't download the update: the server answered %lu.",
        L"Couldn't save the download (error %lu).",
        L"The downloaded file doesn't match the published one (SHA-256), so it wasn't started.",
        L"Setup needs an administrator's permission. Ask an administrator of this computer to enter their password and try again.",
        L"Couldn't start setup (error %lu).",
        L"Try again",
        L"Cancel",
        L"Close",
        L"What's new:",
        L"Ask an administrator of this computer to update the app: only they can install it.",
        L"<a href=\"install\">Install now</a> (needs an administrator's password)",
        L"Got it",
    };

    const wchar_t* const kUk[tCount] = {
        L"Доступна нова версія Exercise Unlock %s",
        L"Встановлено: %s. Оновлення зберігає обліковий запис користувача, пароль і калібрування.",
        L"Завантажити й встановити\nWindows попросить дозволу адміністратора.",
        L"Пізніше",
        L"Завантаження Exercise Unlock %s",
        L"З'єднання…",
        L"%.1f з %.1f МБ",
        L"%.1f МБ",
        L"Чекаємо дозволу адміністратора…",
        L"Оновлення не встановлено",
        L"Не вдалося завантажити оновлення (помилка %lu). Перевірте з'єднання з інтернетом і спробуйте ще раз.",
        L"Не вдалося завантажити оновлення: сервер відповів %lu.",
        L"Не вдалося зберегти файл (помилка %lu).",
        L"Завантажений файл не збігається з опублікованим (SHA-256), тому його не запущено.",
        L"Інсталяції потрібен дозвіл адміністратора. Попросіть адміністратора ввести його пароль і спробуйте ще раз.",
        L"Не вдалося запустити інсталяцію (помилка %lu).",
        L"Спробувати ще раз",
        L"Скасувати",
        L"Закрити",
        L"Що нового:",
        L"Попросіть адміністратора цього комп'ютера оновити програму: встановити її може лише він.",
        L"<a href=\"install\">Встановити зараз</a> (потрібен пароль адміністратора)",
        L"Зрозуміло",
    };

    bool g_uk = false;
    const wchar_t* T(Txt id) { return (g_uk ? kUk : kEn)[id]; }

    std::wstring Fmt(const wchar_t* fmt, ...)
    {
        wchar_t buf[512];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
        va_end(args);
        return buf;
    }

    // ---- download ----

    // The installer download, on a worker thread; the window polls it.
    struct Job
    {
        std::wstring url, path, sha256;
        std::atomic<uint64_t> got{ 0 }, total{ 0 };
        std::atomic<bool> cancel{ false }, done{ false };
        Txt error = tCount;                      // tCount: downloaded and verified
        DWORD code = 0;                          // the number in the error text
        HANDLE held = INVALID_HANDLE_VALUE;      // the verified file, see Work()
    };

    struct Inet
    {
        HINTERNET h;
        ~Inet() { if (h) WinHttpCloseHandle(h); }
    };

    // GET job.url -> job.path. tCount on success.
    Txt Fetch(Job& job)
    {
        URL_COMPONENTSW uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256]{}, path[2048]{};
        uc.lpszHostName = host;  uc.dwHostNameLength = (DWORD)std::size(host);
        uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = (DWORD)std::size(path);
        if (!WinHttpCrackUrl(job.url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
        {
            job.code = ERROR_INVALID_PARAMETER;
            return tNetError;
        }

        const std::wstring ua = std::wstring(L"ExerciseUnlock/") + ExerciseUnlock::kVersionString;
        Inet session{ WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
        if (!session.h) { job.code = GetLastError(); return tNetError; }
        WinHttpSetTimeouts(session.h, 10000, 10000, 30000, 30000);
        Inet conn{ WinHttpConnect(session.h, host, uc.nPort, 0) };
        Inet req{ conn.h ? WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                         : nullptr };
        if (!req.h ||
            !WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req.h, nullptr))
        {
            job.code = GetLastError();
            return tNetError;
        }

        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) { job.code = status; return tHttpError; }
        uint64_t total = 0;
        sz = sizeof(total);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                                WINHTTP_HEADER_NAME_BY_INDEX, &total, &sz, WINHTTP_NO_HEADER_INDEX))
            job.total = total;

        // FILE_SHARE_DELETE: a cancelled download is deleted while this
        // thread may still be writing it.
        HANDLE f = CreateFileW(job.path.c_str(), GENERIC_WRITE, FILE_SHARE_DELETE, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { job.code = GetLastError(); return tWriteError; }
        std::vector<char> buf(1 << 16);
        Txt result = tCount;
        for (;;)
        {
            DWORD n = 0, wrote = 0;
            if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &n))
            {
                job.code = GetLastError();
                result = tNetError;
                break;
            }
            if (n == 0 || job.cancel) break;
            if (!WriteFile(f, buf.data(), n, &wrote, nullptr) || wrote != n)
            {
                job.code = GetLastError();
                result = tWriteError;
                break;
            }
            job.got += n;
        }
        CloseHandle(f);
        if (result == tCount && total && job.got != total)
        {
            job.code = ERROR_HANDLE_EOF;
            result = tNetError;
        }
        return result;
    }

    std::wstring Sha256Hex(HANDLE f)
    {
        std::wstring hex;
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return hex;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0)
        {
            std::vector<UCHAR> buf(1 << 16);
            bool ok = true;
            for (;;)
            {
                DWORD n = 0;
                ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) != FALSE;
                if (!ok || n == 0) break;
                BCryptHashData(hash, buf.data(), n, 0);
            }
            UCHAR digest[32];
            if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0)
                for (UCHAR b : digest) hex += Fmt(L"%02x", b);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return hex;
    }

    void Work(Job* job)
    {
        job->error = Fetch(*job);
        if (job->error == tCount && !job->cancel)
        {
            // Checked through a handle that shares neither writing nor
            // deleting, kept open until setup has started: %TEMP% is the
            // user's, and what was checked is what Windows elevates.
            HANDLE f = CreateFileW(job->path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (f == INVALID_HANDLE_VALUE)
            {
                job->code = GetLastError();
                job->error = tWriteError;
            }
            else if (!job->sha256.empty() && _wcsicmp(Sha256Hex(f).c_str(), job->sha256.c_str()) != 0)
            {
                CloseHandle(f);
                job->error = tBadHash;
            }
            else
                job->held = f;
        }
        if (job->error != tCount) DeleteFileW(job->path.c_str());
        job->done = true;
    }

    // Setup started from here updates silently (its progress window only,
    // no questions); /FROMAPP=1 lets it fall back to the wizard.
    DWORD RunSetup(HWND owner, const std::wstring& exe)
    {
        const std::wstring params = std::wstring(L"/SILENT /LANG=") + (g_uk ? L"uk" : L"en") + L" /FROMAPP=1";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.hwnd = owner;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&sei) ? ERROR_SUCCESS : GetLastError();
    }

    // ---- window ----

    enum Id { idInstall = 100, idLater, idRetry };
    enum class Stage { Offer, Downloading, Starting, Failed, Done };

    Stage g_stage = Stage::Offer;
    // Never freed: after a cancel the download thread may outlive the window.
    Job& g_job = *new Job;
    std::thread g_worker;
    std::wstring g_version, g_footer, g_main, g_content;
    HICON g_icon = nullptr, g_iconSmall = nullptr;

    HRESULT CALLBACK Callback(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR);

    TASKDIALOGCONFIG Page(DWORD flags)
    {
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof(c);
        c.hInstance = GetModuleHandleW(nullptr);
        c.dwFlags = flags | TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_CALLBACK_TIMER;
        c.pszWindowTitle = L"Exercise Unlock";
        c.hMainIcon = g_icon;
        c.pszMainInstruction = g_main.c_str();
        c.pszContent = g_content.c_str();
        c.pszFooter = g_footer.empty() ? nullptr : g_footer.c_str();
        c.pfCallback = Callback;
        c.cxWidth = 260;   // dialog units: the heading with its version on one line
        return c;
    }

    void ShowDownloading(HWND hwnd)
    {
        g_main = Fmt(T(tDownloading), g_version.c_str());
        g_content = T(tConnecting);
        // Own buttons, not the common ones: those speak Windows' language.
        static TASKDIALOG_BUTTON cancel[1];
        cancel[0] = { IDCANCEL, T(tCancel) };
        TASKDIALOGCONFIG c = Page(TDF_USE_HICON_MAIN | TDF_SHOW_PROGRESS_BAR);
        c.pButtons = cancel;
        c.cButtons = 1;
        SendMessageW(hwnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c);
    }

    void ShowFailed(HWND hwnd, const std::wstring& why)
    {
        g_stage = Stage::Failed;
        g_main = T(tFailed);
        g_content = why;
        static TASKDIALOG_BUTTON buttons[2];
        buttons[0] = { idRetry, T(tRetry) };
        buttons[1] = { IDCANCEL, T(tClose) };
        TASKDIALOGCONFIG c = Page(0);
        c.pszMainIcon = TD_WARNING_ICON;
        c.pButtons = buttons;
        c.cButtons = 2;
        SendMessageW(hwnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c);
    }

    void StartDownload(HWND hwnd)
    {
        g_job.got = 0;
        g_job.total = 0;
        g_job.cancel = false;
        g_job.done = false;
        g_job.error = tCount;
        g_job.code = 0;
        g_stage = Stage::Downloading;
        ShowDownloading(hwnd);
        g_worker = std::thread(Work, &g_job);
    }

    void StartSetup(HWND hwnd)
    {
        g_stage = Stage::Starting;
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, 1000, 0);
        SendMessageW(hwnd, TDM_UPDATE_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)T(tStarting));
        RedrawWindow(hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
        const DWORD err = RunSetup(hwnd, g_job.path);
        if (err != ERROR_SUCCESS)
        {
            // The file stays checked and held: "Try again" only asks again.
            ShowFailed(hwnd, err == ERROR_CANCELLED ? T(tCancelled) : Fmt(T(tRunError), err));
            return;
        }
        CloseHandle(g_job.held);
        g_job.held = INVALID_HANDLE_VALUE;
        g_stage = Stage::Done;
        PostMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
    }

    void Poll(HWND hwnd)
    {
        if (g_job.done)
        {
            g_worker.join();
            if (g_job.error == tCount)
                StartSetup(hwnd);
            else
                ShowFailed(hwnd, Fmt(T(g_job.error), g_job.code));
            return;
        }
        const uint64_t got = g_job.got, total = g_job.total;
        if (!got) return;
        const double mb = 1024.0 * 1024.0;
        const std::wstring text = total ? Fmt(T(tProgress), (double)got / mb, (double)total / mb)
                                        : Fmt(T(tProgressMb), (double)got / mb);
        SendMessageW(hwnd, TDM_UPDATE_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)text.c_str());
        if (total) SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, (WPARAM)(got * 1000 / total), 0);
    }

    HRESULT CALLBACK Callback(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR)
    {
        switch (msg)
        {
        case TDN_CREATED:
            // Whose window this is: the app icon on the title bar and the
            // taskbar. In front, since it opens on its own.
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case TDN_NAVIGATED:
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
            break;
        case TDN_BUTTON_CLICKED:
            switch (wp)
            {
            case idInstall:
                StartDownload(hwnd);
                return S_FALSE;
            case idRetry:
                if (g_job.held == INVALID_HANDLE_VALUE)
                    StartDownload(hwnd);
                else
                {
                    ShowDownloading(hwnd);
                    StartSetup(hwnd);
                }
                return S_FALSE;
            case IDCANCEL:
                if (g_stage == Stage::Starting) return S_FALSE;
                if (g_stage == Stage::Downloading) g_job.cancel = true;
                return S_OK;
            }
            return S_OK;   // Later
        case TDN_TIMER:
            if (g_stage == Stage::Downloading) Poll(hwnd);
            break;
        case TDN_HYPERLINK_CLICKED:
            if (wcscmp(reinterpret_cast<LPCWSTR>(lp), L"install") == 0)
                StartDownload(hwnd);   // "Install now" in the child's window
            else
                ShellExecuteW(hwnd, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        return S_OK;
    }

    // An https URL with nothing that could break out of its quotes.
    bool PlainHttps(const std::wstring& s)
    {
        return s.rfind(L"https://", 0) == 0 && s.find_first_of(L"\"<> \t") == std::wstring::npos;
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    // One window per session, however often the service asks.
    CreateMutexW(nullptr, FALSE, L"Local\\ExerciseUnlockUpdate");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 1;

    wchar_t lang[16]{};
    DWORD cb = sizeof(lang);
    g_uk = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ExerciseUnlock", L"Language", RRF_RT_REG_SZ,
                        nullptr, lang, &cb) == ERROR_SUCCESS
         ? _wcsicmp(lang, L"uk") == 0
         : PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_UKRAINIAN;

    std::wstring notes;
    std::vector<std::wstring> changesEn, changesUk;
    bool ask = false;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i)
    {
        const wchar_t* key = argv[i];
        if (_wcsicmp(key, L"/ask") == 0) { ask = true; continue; }
        if (i + 1 >= argc) break;
        const wchar_t* val = argv[++i];
        if      (_wcsicmp(key, L"/version")   == 0) g_version    = val;
        else if (_wcsicmp(key, L"/download")  == 0) g_job.url    = val;
        else if (_wcsicmp(key, L"/sha256")    == 0) g_job.sha256 = val;
        else if (_wcsicmp(key, L"/notes")     == 0) notes        = val;
        else if (_wcsicmp(key, L"/change-en") == 0) changesEn.push_back(val);
        else if (_wcsicmp(key, L"/change-uk") == 0) changesUk.push_back(val);
        else if (_wcsicmp(key, L"/lang")      == 0) g_uk         = _wcsicmp(val, L"uk") == 0;
    }
    if (argv) LocalFree(argv);
    // The version names the downloaded file: digits and dots only.
    if (g_version.empty() || g_version.find_first_not_of(L"0123456789.") != std::wstring::npos ||
        !PlainHttps(g_job.url))
        return 2;

    // Footer: where the update comes from, a link to the site.
    if (PlainHttps(notes))
    {
        const size_t hostEnd = notes.find(L'/', 8);
        g_footer = L"<a href=\"" + notes + L"\">" + notes.substr(8, hostEnd == std::wstring::npos ? std::wstring::npos : hostEnd - 8) + L"</a>";
    }

    // Installers of earlier updates (one still running just stays).
    wchar_t tmp[MAX_PATH + 1]{};
    GetTempPathW((DWORD)std::size(tmp), tmp);
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((std::wstring(tmp) + L"ExerciseUnlock-Setup-*.exe").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do DeleteFileW((std::wstring(tmp) + fd.cFileName).c_str());
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    g_job.path = std::wstring(tmp) + L"ExerciseUnlock-Setup-" + g_version + L".exe";

    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_LARGE, &g_icon);
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_SMALL, &g_iconSmall);

    g_main = Fmt(T(tAvailable), g_version.c_str());
    const TASKDIALOG_BUTTON install[] = { { idInstall, T(tInstall) }, { idLater, T(tLater) } };
    const TASKDIALOG_BUTTON gotIt[]   = { { idLater, T(tGotIt) } };
    TASKDIALOGCONFIG c{};
    if (ask)
    {
        g_content = std::wstring(T(tAsk)) + L"\n\n" + T(tInstallNow);
        c = Page(TDF_USE_HICON_MAIN);
        c.pButtons = gotIt;
        c.cButtons = (UINT)std::size(gotIt);
    }
    else
    {
        g_content = Fmt(T(tInstalled), ExerciseUnlock::kVersionString);
        const std::vector<std::wstring>& changes = g_uk && !changesUk.empty() ? changesUk : changesEn;
        if (!changes.empty())
        {
            g_content += L"\n\n";
            g_content += T(tChanges);
            for (std::wstring point : changes)
            {
                // Text, not markup: the content shows links.
                for (auto& ch : point)
                    if (ch == L'<') ch = L'\x2039';
                g_content += L"\n•  " + point;
            }
        }
        c = Page(TDF_USE_HICON_MAIN | TDF_USE_COMMAND_LINKS);
        c.pButtons = install;
        c.cButtons = (UINT)std::size(install);
    }
    TaskDialogIndirect(&c, nullptr, nullptr, nullptr);

    if (g_stage == Stage::Done) return 0;
    // Closed without installing: nothing left behind in %TEMP%.
    if (g_worker.joinable()) g_worker.detach();
    if (g_job.held != INVALID_HANDLE_VALUE) CloseHandle(g_job.held);
    DeleteFileW(g_job.path.c_str());
    return 1;
}
