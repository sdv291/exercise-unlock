#include "QrCode.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

// Structure follows the reference description in ISO/IEC 18004 (and
// Project Nayuki's well-known implementation of it): bit stream -> data
// codewords -> Reed-Solomon per block -> interleave -> zigzag placement ->
// the mask with the lowest penalty -> format / version bits.

namespace ExerciseUnlock::Qr
{
    namespace
    {
        constexpr int kMaxVersion = 10;
        // Error correction level M, indexed by version (0 unused).
        constexpr int kEccPerBlock[kMaxVersion + 1] = { -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
        constexpr int kNumBlocks[kMaxVersion + 1]   = { -1,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5 };
        constexpr int kEclFormatBits = 0;   // M = 00
        constexpr char kAlnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

        // Modules left for data + ECC once the function patterns are placed.
        int RawDataModules(int ver)
        {
            int result = (16 * ver + 128) * ver + 64;
            if (ver >= 2)
            {
                const int numAlign = ver / 7 + 2;
                result -= (25 * numAlign - 10) * numAlign - 55;
                if (ver >= 7) result -= 36;
            }
            return result;
        }

        int NumDataCodewords(int ver)
        {
            return RawDataModules(ver) / 8 - kEccPerBlock[ver] * kNumBlocks[ver];
        }

        int AlnumIndex(char c)
        {
            if (c == '\0') return -1;
            const char* p = std::strchr(kAlnum, c);
            return p ? static_cast<int>(p - kAlnum) : -1;
        }

        bool IsAlnum(const std::string& s)
        {
            return std::all_of(s.begin(), s.end(), [](char c) { return AlnumIndex(c) >= 0; });
        }

        int CharCountBits(bool alnum, int ver)
        {
            return alnum ? (ver <= 9 ? 9 : 11) : (ver <= 9 ? 8 : 16);
        }

        // Bits of the single segment at `ver`, -1 if the length can't be encoded.
        int SegmentBits(const std::string& s, bool alnum, int ver)
        {
            const int cc = CharCountBits(alnum, ver);
            if (s.size() >= (static_cast<size_t>(1) << cc)) return -1;
            const int n = static_cast<int>(s.size());
            return 4 + cc + (alnum ? (n / 2) * 11 + (n % 2) * 6 : n * 8);
        }

        struct Bits
        {
            std::vector<uint8_t> v;
            void Append(uint32_t val, int len)
            {
                for (int i = len - 1; i >= 0; --i) v.push_back(static_cast<uint8_t>((val >> i) & 1));
            }
        };

        // GF(2^8) multiply, reducing polynomial x^8 + x^4 + x^3 + x^2 + 1.
        uint8_t GfMul(uint8_t x, uint8_t y)
        {
            int z = 0;
            for (int i = 7; i >= 0; --i)
            {
                z = (z << 1) ^ ((z >> 7) * 0x11D);
                z ^= ((y >> i) & 1) * x;
            }
            return static_cast<uint8_t>(z);
        }

        // Generator polynomial (roots a^0 .. a^(degree-1)), highest term dropped.
        std::vector<uint8_t> RsDivisor(int degree)
        {
            std::vector<uint8_t> result(static_cast<size_t>(degree), 0);
            result.back() = 1;
            uint8_t root = 1;
            for (int i = 0; i < degree; ++i)
            {
                for (size_t j = 0; j < result.size(); ++j)
                {
                    result[j] = GfMul(result[j], root);
                    if (j + 1 < result.size()) result[j] ^= result[j + 1];
                }
                root = GfMul(root, 0x02);
            }
            return result;
        }

        std::vector<uint8_t> RsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor)
        {
            std::vector<uint8_t> result(divisor.size(), 0);
            for (const uint8_t b : data)
            {
                const uint8_t factor = static_cast<uint8_t>(b ^ result.front());
                result.erase(result.begin());
                result.push_back(0);
                for (size_t i = 0; i < result.size(); ++i) result[i] ^= GfMul(divisor[i], factor);
            }
            return result;
        }

        class Matrix
        {
        public:
            explicit Matrix(int version)
                : ver(version), size(version * 4 + 17),
                  mod(static_cast<size_t>(size) * size, 0), fn(static_cast<size_t>(size) * size, 0) {}

            const int ver;
            const int size;
            std::vector<uint8_t> mod;   // 1 = dark
            std::vector<uint8_t> fn;    // 1 = function module (not data)

            bool Get(int x, int y) const { return mod[Idx(x, y)] != 0; }

            void DrawFunctionPatterns()
            {
                for (int i = 0; i < size; ++i)
                {
                    SetFn(6, i, i % 2 == 0);   // timing patterns
                    SetFn(i, 6, i % 2 == 0);
                }
                DrawFinder(3, 3);
                DrawFinder(size - 4, 3);
                DrawFinder(3, size - 4);
                const std::vector<int> pos = AlignmentPositions();
                const int n = static_cast<int>(pos.size());
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j)
                        if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)))
                            DrawAlignment(pos[i], pos[j]);
                DrawFormatBits(0);   // reserves the area; real bits after masking
                DrawVersion();
            }

            // Zigzag over the data area, two columns at a time, right to left.
            void DrawCodewords(const std::vector<uint8_t>& data)
            {
                size_t i = 0;
                for (int right = size - 1; right >= 1; right -= 2)
                {
                    if (right == 6) right = 5;   // skip the vertical timing column
                    for (int vert = 0; vert < size; ++vert)
                    {
                        for (int j = 0; j < 2; ++j)
                        {
                            const int  x = right - j;
                            const bool upward = ((right + 1) & 2) == 0;
                            const int  y = upward ? size - 1 - vert : vert;
                            if (!fn[Idx(x, y)] && i < data.size() * 8)
                            {
                                mod[Idx(x, y)] = static_cast<uint8_t>((data[i >> 3] >> (7 - (i & 7))) & 1);
                                ++i;
                            }
                        }
                    }
                }
                // Remainder bits (0..7) stay light.
            }

            // XOR the data area with mask pattern m; applying it twice undoes it.
            void ApplyMask(int m)
            {
                for (int y = 0; y < size; ++y)
                {
                    for (int x = 0; x < size; ++x)
                    {
                        bool invert = false;
                        switch (m)
                        {
                        case 0: invert = (x + y) % 2 == 0; break;
                        case 1: invert = y % 2 == 0; break;
                        case 2: invert = x % 3 == 0; break;
                        case 3: invert = (x + y) % 3 == 0; break;
                        case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                        case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                        case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                        case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                        default: break;
                        }
                        if (invert && !fn[Idx(x, y)]) mod[Idx(x, y)] ^= 1;
                    }
                }
            }

            void DrawFormatBits(int mask)
            {
                const int data = (kEclFormatBits << 3) | mask;
                int rem = data;
                for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
                const int bits = ((data << 10) | rem) ^ 0x5412;
                auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
                // First copy, around the top-left finder.
                for (int i = 0; i <= 5; ++i) SetFn(8, i, bit(i));
                SetFn(8, 7, bit(6));
                SetFn(8, 8, bit(7));
                SetFn(7, 8, bit(8));
                for (int i = 9; i < 15; ++i) SetFn(14 - i, 8, bit(i));
                // Second copy, split between the other two finders.
                for (int i = 0; i < 8; ++i) SetFn(size - 1 - i, 8, bit(i));
                for (int i = 8; i < 15; ++i) SetFn(8, size - 15 + i, bit(i));
                SetFn(8, size - 8, true);   // always dark
            }

            // Lower is easier to read: ISO 18004 rules N1..N4.
            long Penalty() const
            {
                static const uint8_t kFinderA[11] = { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 };
                static const uint8_t kFinderB[11] = { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 };
                long result = 0;
                for (int pass = 0; pass < 2; ++pass)   // rows, then columns
                {
                    for (int a = 0; a < size; ++a)
                    {
                        auto at = [&](int b) { return pass == 0 ? Get(b, a) : Get(a, b); };
                        int  run  = 0;
                        bool prev = false;
                        for (int b = 0; b < size; ++b)
                        {
                            const bool c = at(b);
                            if (b > 0 && c == prev) { ++run; continue; }
                            if (run >= 5) result += 3 + (run - 5);
                            run  = 1;
                            prev = c;
                        }
                        if (run >= 5) result += 3 + (run - 5);

                        for (int b = 0; b + 10 < size; ++b)
                        {
                            bool matchA = true, matchB = true;
                            for (int k = 0; k < 11; ++k)
                            {
                                const uint8_t c = at(b + k) ? 1 : 0;
                                if (c != kFinderA[k]) matchA = false;
                                if (c != kFinderB[k]) matchB = false;
                            }
                            if (matchA) result += 40;
                            if (matchB) result += 40;
                        }
                    }
                }
                for (int y = 0; y + 1 < size; ++y)
                    for (int x = 0; x + 1 < size; ++x)
                    {
                        const bool c = Get(x, y);
                        if (c == Get(x + 1, y) && c == Get(x, y + 1) && c == Get(x + 1, y + 1)) result += 3;
                    }
                int dark = 0;
                for (const uint8_t m : mod) dark += m;
                const int total = size * size;
                const int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
                result += static_cast<long>(k) * 10;
                return result;
            }

        private:
            size_t Idx(int x, int y) const { return static_cast<size_t>(y) * size + x; }

            void SetFn(int x, int y, bool dark)
            {
                mod[Idx(x, y)] = dark ? 1 : 0;
                fn[Idx(x, y)] = 1;
            }

            void DrawFinder(int cx, int cy)
            {
                for (int dy = -4; dy <= 4; ++dy)
                    for (int dx = -4; dx <= 4; ++dx)
                    {
                        const int dist = (std::max)(std::abs(dx), std::abs(dy));
                        const int x = cx + dx, y = cy + dy;
                        if (x >= 0 && x < size && y >= 0 && y < size) SetFn(x, y, dist != 2 && dist != 4);
                    }
            }

            void DrawAlignment(int cx, int cy)
            {
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                        SetFn(cx + dx, cy + dy, (std::max)(std::abs(dx), std::abs(dy)) != 1);
            }

            std::vector<int> AlignmentPositions() const
            {
                if (ver == 1) return {};
                const int numAlign = ver / 7 + 2;
                const int step = (ver * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2;
                std::vector<int> result;
                for (int i = 0, pos = size - 7; i < numAlign - 1; ++i, pos -= step) result.insert(result.begin(), pos);
                result.insert(result.begin(), 6);
                return result;
            }

            void DrawVersion()
            {
                if (ver < 7) return;
                int rem = ver;
                for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
                const long bits = (static_cast<long>(ver) << 12) | rem;
                for (int i = 0; i < 18; ++i)
                {
                    const bool b = ((bits >> i) & 1) != 0;
                    const int a = size - 11 + i % 3, c = i / 3;
                    SetFn(a, c, b);
                    SetFn(c, a, b);
                }
            }
        };
    }

    std::vector<uint8_t> DataCodewords(const std::string& text, int ver)
    {
        if (ver < 1 || ver > kMaxVersion) return {};
        const bool alnum = IsAlnum(text);
        const size_t capacity = static_cast<size_t>(NumDataCodewords(ver)) * 8;
        const int segBits = SegmentBits(text, alnum, ver);
        if (segBits < 0 || static_cast<size_t>(segBits) > capacity) return {};

        Bits bb;
        bb.Append(alnum ? 0x2 : 0x4, 4);
        bb.Append(static_cast<uint32_t>(text.size()), CharCountBits(alnum, ver));
        if (alnum)
        {
            size_t i = 0;
            for (; i + 1 < text.size(); i += 2)
                bb.Append(static_cast<uint32_t>(AlnumIndex(text[i]) * 45 + AlnumIndex(text[i + 1])), 11);
            if (i < text.size()) bb.Append(static_cast<uint32_t>(AlnumIndex(text[i])), 6);
        }
        else
        {
            for (const char c : text) bb.Append(static_cast<uint8_t>(c), 8);
        }
        bb.Append(0, static_cast<int>((std::min)(static_cast<size_t>(4), capacity - bb.v.size())));   // terminator
        bb.Append(0, static_cast<int>((8 - bb.v.size() % 8) % 8));
        for (uint32_t pad = 0xEC; bb.v.size() < capacity; pad ^= 0xEC ^ 0x11) bb.Append(pad, 8);

        std::vector<uint8_t> out(bb.v.size() / 8, 0);
        for (size_t i = 0; i < bb.v.size(); ++i) out[i >> 3] |= static_cast<uint8_t>(bb.v[i] << (7 - (i & 7)));
        return out;
    }

    std::vector<uint8_t> EccCodewords(const std::vector<uint8_t>& data, int degree)
    {
        return RsRemainder(data, RsDivisor(degree));
    }

    bool Encode(const std::string& text, Code& out)
    {
        const bool alnum = IsAlnum(text);
        int ver = 1;
        for (; ver <= kMaxVersion; ++ver)
        {
            const int bits = SegmentBits(text, alnum, ver);
            if (bits >= 0 && bits <= NumDataCodewords(ver) * 8) break;
        }
        if (ver > kMaxVersion) return false;
        const std::vector<uint8_t> data = DataCodewords(text, ver);

        // Split into blocks, add Reed-Solomon to each, interleave.
        const int numBlocks = kNumBlocks[ver], eccLen = kEccPerBlock[ver];
        const int raw = RawDataModules(ver) / 8;
        const int numShort = numBlocks - raw % numBlocks;
        const int shortLen = raw / numBlocks;
        const std::vector<uint8_t> divisor = RsDivisor(eccLen);
        std::vector<std::vector<uint8_t>> blocks;
        for (int i = 0, k = 0; i < numBlocks; ++i)
        {
            const int len = shortLen - eccLen + (i < numShort ? 0 : 1);
            std::vector<uint8_t> block(data.begin() + k, data.begin() + k + len);
            k += len;
            const std::vector<uint8_t> ecc = RsRemainder(block, divisor);
            if (i < numShort) block.push_back(0);   // placeholder, skipped below
            block.insert(block.end(), ecc.begin(), ecc.end());
            blocks.push_back(std::move(block));
        }
        std::vector<uint8_t> all;
        for (size_t i = 0; i < blocks[0].size(); ++i)
            for (int j = 0; j < numBlocks; ++j)
                if (i != static_cast<size_t>(shortLen - eccLen) || j >= numShort) all.push_back(blocks[j][i]);

        Matrix m(ver);
        m.DrawFunctionPatterns();
        m.DrawCodewords(all);
        int  best = 0;
        long bestPenalty = LONG_MAX;
        for (int mask = 0; mask < 8; ++mask)
        {
            m.ApplyMask(mask);
            m.DrawFormatBits(mask);
            const long p = m.Penalty();
            if (p < bestPenalty) { bestPenalty = p; best = mask; }
            m.ApplyMask(mask);
        }
        m.ApplyMask(best);
        m.DrawFormatBits(best);

        out.size = m.size;
        out.version = ver;
        out.mask = best;
        out.dark = m.mod;
        return true;
    }
}
