#pragma once

#include <cstdint>

namespace ExerciseUnlock
{
    // Semver. Bump before every release — the UpdateChecker uses
    // these numbers to compare against the server manifest.
    inline constexpr int kVersionMajor = 1;
    inline constexpr int kVersionMinor = 1;
    inline constexpr int kVersionPatch = 0;

    inline constexpr wchar_t kVersionString[] = L"1.1.0";

    // Pack major.minor.patch into one comparable uint32 (M<<16|m<<8|p).
    inline constexpr uint32_t PackVersion(int M, int m, int p)
    {
        return (uint32_t)((M & 0xFFFF) << 16 | (m & 0xFF) << 8 | (p & 0xFF));
    }
    inline constexpr uint32_t kVersionPacked =
        PackVersion(kVersionMajor, kVersionMinor, kVersionPatch);
}
