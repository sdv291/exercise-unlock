#include "Vault.h"

#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>
#include <vector>
#include <cstring>
#include <cwctype>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace ExerciseUnlock::Vault
{
    // Format evolution:
    //   v1 — single credential (domain / user / password)
    //   v2 — list of credentials [count, [d/u/p]...] (v1.1)
    // Loader accepts both; writer always emits v2.
    static constexpr uint32_t kMagic   = 0x4B4C5556;  // 'EULK-VLT' first 4
    static constexpr uint16_t kVersion = 2;

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

    std::wstring VaultPath()
    {
        return ProgramDataDir() + L"\\vault.dat";
    }

    static HRESULT EnsureDirWithAcl(const std::wstring& dir)
    {
        const wchar_t* sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        sa.lpSecurityDescriptor = sd;

        BOOL ok = CreateDirectoryW(dir.c_str(), &sa);
        DWORD err = GetLastError();
        LocalFree(sd);
        if (!ok && err != ERROR_ALREADY_EXISTS) return HRESULT_FROM_WIN32(err);
        return S_OK;
    }

    // ---- byte-stream helpers ----
    struct Writer
    {
        std::vector<BYTE> out;
        void u16(uint16_t v) {
            out.push_back((BYTE)(v & 0xFF));
            out.push_back((BYTE)((v >> 8) & 0xFF));
        }
        void u32(uint32_t v) {
            for (int i = 0; i < 4; ++i) out.push_back((BYTE)((v >> (i*8)) & 0xFF));
        }
        void wstr(const std::wstring& s) {
            u32((uint32_t)s.size());
            const BYTE* p = (const BYTE*)s.data();
            out.insert(out.end(), p, p + s.size() * sizeof(wchar_t));
        }
    };

    struct Reader
    {
        const BYTE* d; size_t n; size_t p;
        bool u16(uint16_t& v) {
            if (p + 2 > n) return false;
            v = (uint16_t)(d[p] | (d[p+1] << 8)); p += 2; return true;
        }
        bool u32(uint32_t& v) {
            if (p + 4 > n) return false;
            v = d[p] | (d[p+1] << 8) | (d[p+2] << 16) | (d[p+3] << 24);
            p += 4; return true;
        }
        bool wstr(std::wstring& s) {
            uint32_t k;
            if (!u32(k) || k > 1024) return false;
            if (p + k * sizeof(wchar_t) > n) return false;
            s.assign((const wchar_t*)(d + p), k);
            p += k * sizeof(wchar_t);
            return true;
        }
    };

    static bool IEqual(const std::wstring& a, const std::wstring& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (towlower(a[i]) != towlower(b[i])) return false;
        return true;
    }

    // ---- DPAPI I/O of the whole vector<Credentials> ----
    static HRESULT WriteEncrypted(const std::vector<Credentials>& entries)
    {
        HRESULT hr = EnsureDirWithAcl(ProgramDataDir());
        if (FAILED(hr)) return hr;

        Writer w;
        w.u32(kMagic);
        w.u16(kVersion);
        w.u16(0);
        w.u32((uint32_t)entries.size());
        for (const auto& e : entries) {
            w.wstr(e.domain);
            w.wstr(e.username);
            w.wstr(e.password);
        }

        DATA_BLOB in{ (DWORD)w.out.size(), w.out.data() };
        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"ExerciseUnlock vault",
                              nullptr, nullptr, nullptr,
                              CRYPTPROTECT_LOCAL_MACHINE, &out))
        {
            SecureZeroMemory(w.out.data(), w.out.size());
            return HRESULT_FROM_WIN32(GetLastError());
        }
        SecureZeroMemory(w.out.data(), w.out.size());

        HANDLE f = CreateFileW(VaultPath().c_str(),
                               GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            LocalFree(out.pbData);
            return HRESULT_FROM_WIN32(err);
        }
        DWORD wrote = 0;
        BOOL ok = WriteFile(f, out.pbData, out.cbData, &wrote, nullptr) &&
                  wrote == out.cbData;
        CloseHandle(f);
        LocalFree(out.pbData);
        return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    static HRESULT ReadDecrypted(std::vector<Credentials>& out)
    {
        out.clear();
        HANDLE f = CreateFileW(VaultPath().c_str(),
                               GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
            CloseHandle(f); return E_FAIL;
        }
        std::vector<BYTE> cipher((size_t)size.QuadPart);
        DWORD read = 0;
        BOOL ok = ReadFile(f, cipher.data(), (DWORD)cipher.size(), &read, nullptr) &&
                  read == cipher.size();
        CloseHandle(f);
        if (!ok) return HRESULT_FROM_WIN32(GetLastError());

        DATA_BLOB in{ (DWORD)cipher.size(), cipher.data() };
        DATA_BLOB plain{};
        LPWSTR desc = nullptr;
        if (!CryptUnprotectData(&in, &desc, nullptr, nullptr, nullptr,
                                CRYPTPROTECT_LOCAL_MACHINE, &plain))
            return HRESULT_FROM_WIN32(GetLastError());
        if (desc) LocalFree(desc);

        Reader r{ plain.pbData, plain.cbData, 0 };
        HRESULT hr = S_OK;
        uint32_t magic; uint16_t ver, pad;
        if (!r.u32(magic) || magic != kMagic) { hr = E_FAIL; goto done; }
        if (!r.u16(ver))                       { hr = E_FAIL; goto done; }
        if (!r.u16(pad))                       { hr = E_FAIL; goto done; }

        if (ver == 1) {
            // Legacy single-entry vault: promote to a 1-item list.
            Credentials c;
            if (!r.wstr(c.domain) || !r.wstr(c.username) || !r.wstr(c.password)) {
                hr = E_FAIL; goto done;
            }
            out.push_back(std::move(c));
        }
        else if (ver == kVersion) {
            uint32_t n = 0;
            if (!r.u32(n) || n > 64) { hr = E_FAIL; goto done; }
            out.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                Credentials c;
                if (!r.wstr(c.domain) || !r.wstr(c.username) || !r.wstr(c.password)) {
                    hr = E_FAIL; goto done;
                }
                out.push_back(std::move(c));
            }
        }
        else {
            hr = E_FAIL;
        }

    done:
        SecureZeroMemory(plain.pbData, plain.cbData);
        LocalFree(plain.pbData);
        return hr;
    }

    // ---- public API ----
    HRESULT Store(const Credentials& creds)
    {
        std::vector<Credentials> all;
        HRESULT hr = ReadDecrypted(all);
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            all.clear();  // corrupt / missing — start fresh

        bool replaced = false;
        for (auto& e : all) {
            if (IEqual(e.username, creds.username)) {
                e = creds; replaced = true; break;
            }
        }
        if (!replaced) all.push_back(creds);
        return WriteEncrypted(all);
    }

    HRESULT Load(const std::wstring& username, Credentials& out)
    {
        out = {};
        std::vector<Credentials> all;
        HRESULT hr = ReadDecrypted(all);
        if (FAILED(hr)) return hr;
        for (const auto& e : all) {
            if (IEqual(e.username, username)) { out = e; return S_OK; }
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    HRESULT Load(Credentials& out)
    {
        out = {};
        std::vector<Credentials> all;
        HRESULT hr = ReadDecrypted(all);
        if (FAILED(hr)) return hr;
        if (all.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        out = all.front();
        return S_OK;
    }

    HRESULT LoadAll(std::vector<Credentials>& out)
    {
        return ReadDecrypted(out);
    }

    HRESULT Remove(const std::wstring& username)
    {
        std::vector<Credentials> all;
        HRESULT hr = ReadDecrypted(all);
        if (FAILED(hr)) return hr;
        size_t before = all.size();
        for (auto it = all.begin(); it != all.end(); ) {
            if (IEqual(it->username, username)) it = all.erase(it);
            else ++it;
        }
        if (all.size() == before) return S_OK;
        if (all.empty()) return Clear();
        return WriteEncrypted(all);
    }

    bool Exists()
    {
        return GetFileAttributesW(VaultPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    HRESULT Clear()
    {
        if (!DeleteFileW(VaultPath().c_str())) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) return S_OK;
            return HRESULT_FROM_WIN32(err);
        }
        return S_OK;
    }
}
