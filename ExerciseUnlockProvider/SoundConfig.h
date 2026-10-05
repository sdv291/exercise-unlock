#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace ExerciseUnlock
{
    // Provider-side sound configuration.
    //
    // Reads %ProgramData%\ExerciseUnlock\sounds.ini on construction
    // and on every Reload() call. The provider calls Reload() each
    // RefreshStatus so edits to sounds.ini take effect without a
    // sign-out — handy while picking files.
    //
    // Config format (standard INI, UTF-16 or UTF-8):
    //
    //   [sounds]
    //   library        = C:\ProgramData\ExerciseUnlock\sounds
    //   squat_ready    = ready1.wav, ready2.wav
    //   pushup_ready   = ready_pu.wav
    //   squat_counted  = ding1.wav, ding2.wav, ding3.wav
    //   pushup_counted = ding_pu.wav
    //   error          = oops.wav
    //
    // Each list is comma-separated; whitespace around entries is
    // trimmed. A missing or empty value leaves the slot empty, and
    // the provider falls back to a MessageBeep. An entry without
    // a drive letter is resolved against `library`; an absolute
    // path is used verbatim.
    //
    // When a list has multiple files, PickOne() returns a uniformly
    // random pick each time it fires, so a batch of 5 reps can play
    // 5 different dings instead of the same one 5 times.
    class SoundConfig
    {
    public:
        enum Slot
        {
            SlotSquatReady   = 0,
            SlotPushupReady  = 1,
            SlotSquatCounted = 2,
            SlotPushupCounted = 3,
            SlotError        = 4,
            SlotCount
        };

        SoundConfig();

        // Re-read sounds.ini. Called by Credential before each
        // potential beep so hand-edits appear live.
        void Reload();

        // Full path to one of the files in `slot`, chosen at
        // random. Empty string if that slot has no configured
        // files (caller then uses its MessageBeep fallback).
        std::wstring PickOne(Slot slot) const;

    private:
        static std::wstring ConfigPath();
        static std::vector<std::wstring> ParseList(const std::wstring& csv);
        std::wstring ResolveAgainstLibrary(const std::wstring& entry) const;

        std::wstring              m_library;
        std::vector<std::wstring> m_slots[SlotCount];
        FILETIME                  m_lastWrite{};
    };
}
