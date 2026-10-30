#pragma once

#include <windows.h>

#include <string>
#include <vector>

// The "Settings" window: every value of config.ini and sounds.ini, grouped
// into pages. Comments and the file's encoding survive a save: only the
// values that changed are written, through the profile API.

namespace ExerciseUnlock::Calib
{
    struct SettingsRequest
    {
        HINSTANCE inst = nullptr;
        HWND      owner = nullptr;          // disabled while the window is open
        bool      uk = false;
        std::wstring configPath, soundsPath;
        std::vector<std::wstring> cameras;  // suggestions for the preferred camera

        // Developer option: render this page to a .bmp and close.
        std::wstring snapshotPath;
        int          snapshotPage = 0;
    };

    struct SettingsResult
    {
        bool saved = false;          // something was written
        bool serviceConfig = false;  // ... to config.ini: the service reads it at start
    };

    // Modal: returns when the window is closed.
    SettingsResult ShowSettings(const SettingsRequest& request);
}
