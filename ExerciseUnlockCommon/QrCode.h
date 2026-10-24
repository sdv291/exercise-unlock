#pragma once

// Minimal QR Code encoder (ISO/IEC 18004) for the lock-screen pairing code:
// one segment (alphanumeric when every character allows it, else byte),
// error correction level M, versions 1-10 (up to 311 alphanumeric / 213
// byte characters). Pure computation, no allocation beyond std::vector.

#include <cstdint>
#include <string>
#include <vector>

namespace ExerciseUnlock::Qr
{
    struct Code
    {
        int size = 0;                  // modules per side (21 for version 1)
        int version = 0;
        int mask = 0;
        std::vector<uint8_t> dark;     // size*size, row-major, 1 = dark module

        bool At(int x, int y) const { return dark[static_cast<size_t>(y) * size + x] != 0; }
    };

    // false when the text doesn't fit version 10.
    bool Encode(const std::string& text, Code& out);

    // Exposed for the unit test: data + error-correction codewords of a
    // version / payload, before placement in the matrix.
    std::vector<uint8_t> DataCodewords(const std::string& text, int version);
    std::vector<uint8_t> EccCodewords(const std::vector<uint8_t>& data, int degree);
}
