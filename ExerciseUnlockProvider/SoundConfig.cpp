#include "SoundConfig.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ExerciseUnlock
{
    static constexpr const wchar_t* kSection = L"sounds";

    // The INI key for each slot — matches the user-facing names
    // documented in SoundConfig.h.
    static const wchar_t* SlotKey(SoundConfig::Slot s)
    {
        switch (s)
        {
        case SoundConfig::SlotSquatReady:    return L"squat_ready";
        case SoundConfig::SlotPushupReady:   return L"pushup_ready";
        case SoundConfig::SlotSquatCounted:  return L"squat_counted";
        case SoundConfig::SlotPushupCounted: return L"pushup_counted";
        case SoundConfig::SlotError:         return L"error";
        default:                             return L"";
        }
    }

    std::wstring SoundConfig::ConfigPath()
    {
        wchar_t buf[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                       nullptr, SHGFP_TYPE_CURRENT, buf)))
        {
            return std::wstring(buf) + L"\\ExerciseUnlock\\sounds.ini";
        }
        return L"C:\\ProgramData\\ExerciseUnlock\\sounds.ini";
    }

    // True for chars we eat silently around and inside an entry —
    // bare whitespace, delimiters that aren't part of the format
    // but are easy for a human to type by mistake. Covers writing
    // the list as "[a.wav, b.wav]" or "'a.wav', 'b.wav'" without
    // the brackets/quotes ending up baked into the resolved path.
    static bool IsNoiseChar(wchar_t c)
    {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
               c == L'['  || c == L']'  ||
               c == L'('  || c == L')'  ||
               c == L'{'  || c == L'}'  ||
               c == L'\"' || c == L'\'';
    }

    std::vector<std::wstring> SoundConfig::ParseList(const std::wstring& csv)
    {
        std::vector<std::wstring> out;
        size_t i = 0;
        while (i < csv.size())
        {
            // skip leading whitespace / commas / stray noise chars
            while (i < csv.size() &&
                   (csv[i] == L',' || IsNoiseChar(csv[i])))
                ++i;
            if (i >= csv.size()) break;
            // collect until next comma
            size_t start = i;
            while (i < csv.size() && csv[i] != L',') ++i;
            size_t end = i;
            // trim trailing noise chars
            while (end > start && IsNoiseChar(csv[end - 1])) --end;
            if (end > start)
                out.emplace_back(csv.substr(start, end - start));
        }
        return out;
    }

    std::wstring SoundConfig::ResolveAgainstLibrary(const std::wstring& entry) const
    {
        // Absolute path (drive letter, UNC, or starting with '\\')
        // is used as-is; otherwise prepend the library directory.
        if (entry.size() >= 2 && (entry[1] == L':' ||
            (entry[0] == L'\\' && entry[1] == L'\\')))
        {
            return entry;
        }
        if (m_library.empty()) return entry;
        std::wstring p = m_library;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/')
            p += L'\\';
        p += entry;
        return p;
    }

    SoundConfig::SoundConfig()
    {
        // Seed rand once per DLL load — PickOne uses rand() for
        // the random choice. Not cryptographic; good enough for
        // "don't play the same ding every rep".
        std::srand(static_cast<unsigned>(::GetTickCount64()));
        Reload();
    }

    void SoundConfig::Reload()
    {
        const std::wstring path = ConfigPath();

        // Skip the full re-read when the file hasn't been touched
        // since last load. Saves a dozen GetPrivateProfileString
        // calls per poll (every 500ms).
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        {
            if (fad.ftLastWriteTime.dwLowDateTime  == m_lastWrite.dwLowDateTime &&
                fad.ftLastWriteTime.dwHighDateTime == m_lastWrite.dwHighDateTime)
            {
                return;   // unchanged — in-memory copy still valid
            }
            m_lastWrite = fad.ftLastWriteTime;
        }
        else
        {
            // No config file — clear everything, fall back to beeps.
            ZeroMemory(&m_lastWrite, sizeof(m_lastWrite));
            m_library.clear();
            for (int i = 0; i < SlotCount; ++i) m_slots[i].clear();
            return;
        }

        auto readStr = [&](const wchar_t* key) -> std::wstring
        {
            // GetPrivateProfileString with a big buffer. 4 KB is
            // plenty for a comma list of a dozen file names.
            wchar_t buf[4096]{};
            GetPrivateProfileStringW(kSection, key, L"",
                                     buf, _countof(buf), path.c_str());
            return std::wstring(buf);
        };

        // Library directory (default: same folder as sounds.ini).
        std::wstring lib = readStr(L"library");
        if (lib.empty())
        {
            // Default: the "sounds" folder next to the ini.
            wchar_t buf[MAX_PATH]{};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA,
                                           nullptr, SHGFP_TYPE_CURRENT, buf)))
            {
                lib = std::wstring(buf) + L"\\ExerciseUnlock\\sounds";
            }
        }
        m_library = lib;

        for (int i = 0; i < SlotCount; ++i)
        {
            const auto entries = ParseList(readStr(SlotKey(static_cast<Slot>(i))));
            std::vector<std::wstring> resolved;
            resolved.reserve(entries.size());
            for (const auto& e : entries)
                resolved.emplace_back(ResolveAgainstLibrary(e));
            m_slots[i] = std::move(resolved);
        }
    }

    std::wstring SoundConfig::PickOne(Slot slot) const
    {
        if (slot < 0 || slot >= SlotCount) return L"";
        const auto& v = m_slots[slot];
        if (v.empty()) return L"";
        if (v.size() == 1) return v[0];
        return v[static_cast<size_t>(std::rand()) % v.size()];
    }
}
