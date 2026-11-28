//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "Etc2Codec.hpp"
#include "MobileInternal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

namespace mirage::mobile {

namespace {

constexpr int32_t kEtc1ModTable[8][2] = {
    {2, 8},
    {5, 17},
    {9, 29},
    {13, 42},
    {18, 60},
    {24, 80},
    {33, 106},
    {47, 183},
};

constexpr int32_t kEtc2ThDist[8] = {3, 6, 11, 16, 23, 32, 41, 64};

constexpr int32_t kEacModTable[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},
    {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},
    {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},
    {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},
    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},
    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

using Rgb3 = std::array<uint8_t, 3>;
using RgbBlock = std::array<Rgb3, 16>;
using RgbaBlock = std::array<std::array<uint8_t, 4>, 16>;

inline uint8_t ClampU8(int32_t v) noexcept {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

inline int32_t SignExt3(uint8_t v) noexcept {
    return (v & 0x04U) ? (static_cast<int32_t>(v) - 8) : static_cast<int32_t>(v);
}

inline uint8_t Extend4To8(uint8_t v) noexcept { return static_cast<uint8_t>((v << 4) | v); }
inline uint8_t Extend5To8(uint8_t v) noexcept { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
inline uint8_t Extend6To8(uint8_t v) noexcept { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
inline uint8_t Extend7To8(uint8_t v) noexcept { return static_cast<uint8_t>((v << 1) | (v >> 6)); }

inline uint8_t Quantize4(uint8_t v) noexcept { return static_cast<uint8_t>((static_cast<uint32_t>(v) * 15U + 127U) / 255U); }
inline uint8_t Quantize5(uint8_t v) noexcept { return static_cast<uint8_t>((static_cast<uint32_t>(v) * 31U + 127U) / 255U); }
inline uint8_t Quantize6(uint8_t v) noexcept { return static_cast<uint8_t>((static_cast<uint32_t>(v) * 63U + 127U) / 255U); }
inline uint8_t Quantize7(uint8_t v) noexcept { return static_cast<uint8_t>((static_cast<uint32_t>(v) * 127U + 127U) / 255U); }

inline uint32_t SqDiff3(const Rgb3& a, const Rgb3& b) noexcept {
    int32_t dr = static_cast<int32_t>(a[0]) - static_cast<int32_t>(b[0]);
    int32_t dg = static_cast<int32_t>(a[1]) - static_cast<int32_t>(b[1]);
    int32_t db = static_cast<int32_t>(a[2]) - static_cast<int32_t>(b[2]);
    return static_cast<uint32_t>(dr * dr + dg * dg + db * db);
}

inline Rgb3 AverageRgb8(
    const RgbBlock& rgb,
    const uint8_t idxs[8],
    uint16_t visible_mask) noexcept {
    uint32_t s0 = 0, s1 = 0, s2 = 0;
    uint32_t count = 0;
    for (int k = 0; k < 8; ++k) {
        uint8_t p = idxs[k];
        if ((visible_mask >> p) & 1U) {
            s0 += rgb[p][0];
            s1 += rgb[p][1];
            s2 += rgb[p][2];
            ++count;
        }
    }
    if (count == 0) {
        for (int k = 0; k < 8; ++k) {
            uint8_t p = idxs[k];
            s0 += rgb[p][0];
            s1 += rgb[p][1];
            s2 += rgb[p][2];
        }
        count = 8;
    }
    uint32_t half = count / 2;
    return {
        static_cast<uint8_t>((s0 + half) / count),
        static_cast<uint8_t>((s1 + half) / count),
        static_cast<uint8_t>((s2 + half) / count),
    };
}

inline RgbaBlock ExtractBlockColMajor(
    const uint8_t* rgba,
    size_t width,
    size_t height,
    size_t bx,
    size_t by) noexcept {
    RgbaBlock out{};
    size_t x0 = bx * 4;
    size_t y0 = by * 4;
    if (x0 + 4 <= width && y0 + 4 <= height) {
        size_t stride = width * 4;
        const uint8_t* row = rgba + (y0 * width + x0) * 4;
        for (size_t y = 0; y < 4; ++y) {
            std::memcpy(out[y].data(), row, 4);
            std::memcpy(out[4 + y].data(), row + 4, 4);
            std::memcpy(out[8 + y].data(), row + 8, 4);
            std::memcpy(out[12 + y].data(), row + 12, 4);
            row += stride;
        }
        return out;
    }
    for (size_t x = 0; x < 4; ++x) {
        size_t sx = std::min(x0 + x, width > 0 ? width - 1 : 0);
        for (size_t y = 0; y < 4; ++y) {
            size_t sy = std::min(y0 + y, height > 0 ? height - 1 : 0);
            const uint8_t* src = rgba + (sy * width + sx) * 4;
            std::memcpy(out[x * 4 + y].data(), src, 4);
        }
    }
    return out;
}

inline void WriteBlockColMajor(
    uint8_t* out,
    size_t width,
    size_t height,
    size_t bx,
    size_t by,
    const RgbaBlock& block) noexcept {
    size_t x0 = bx * 4;
    size_t y0 = by * 4;
    if (x0 + 4 <= width && y0 + 4 <= height) {
        size_t stride = width * 4;
        uint8_t* row = out + (y0 * width + x0) * 4;
        for (size_t y = 0; y < 4; ++y) {
            std::memcpy(row, block[y].data(), 4);
            std::memcpy(row + 4, block[4 + y].data(), 4);
            std::memcpy(row + 8, block[8 + y].data(), 4);
            std::memcpy(row + 12, block[12 + y].data(), 4);
            row += stride;
        }
        return;
    }
    for (size_t x = 0; x < 4; ++x) {
        size_t px = x0 + x;
        if (px >= width) break;
        for (size_t y = 0; y < 4; ++y) {
            size_t py = y0 + y;
            if (py >= height) break;
            std::memcpy(out + (py * width + px) * 4, block[x * 4 + y].data(), 4);
        }
    }
}

uint64_t PackPlanarWord(
    uint8_t ro, uint8_t go, uint8_t bo,
    uint8_t rh, uint8_t gh, uint8_t bh,
    uint8_t rv, uint8_t gv, uint8_t bv) noexcept {
    uint64_t w = 0;
    w |= (static_cast<uint64_t>(ro) & 0x3fULL) << 57;
    w |= static_cast<uint64_t>((go >> 6) & 0x01U) << 56;
    w |= (static_cast<uint64_t>(go) & 0x3fULL) << 49;
    w |= static_cast<uint64_t>((bo >> 5) & 0x01U) << 48;
    w |= static_cast<uint64_t>((bo >> 3) & 0x03U) << 43;
    w |= (static_cast<uint64_t>(bo) & 0x07ULL) << 39;
    w |= static_cast<uint64_t>((rh >> 1) & 0x1fU) << 34;
    w |= 1ULL << 33; // diff = 1
    w |= (static_cast<uint64_t>(rh) & 0x01ULL) << 32;
    w |= (static_cast<uint64_t>(gh) & 0x7fULL) << 25;
    w |= (static_cast<uint64_t>(bh) & 0x3fULL) << 19;
    w |= (static_cast<uint64_t>(rv) & 0x3fULL) << 13;
    w |= (static_cast<uint64_t>(gv) & 0x7fULL) << 6;
    w |= static_cast<uint64_t>(bv) & 0x3fULL;

    // Guard bit 63 so R + dR stays in 0..=31.
    if ((w >> 58) & 1ULL) w |= 1ULL << 63;
    // Guard bit 55 so G + dG stays in 0..=31.
    if ((w >> 50) & 1ULL) w |= 1ULL << 55;
    // Overflow bits 47..45 and 42 so B + dB ALWAYS overflows 0..=31.
    uint8_t ba2 = static_cast<uint8_t>((w >> 43) & 0x03U);
    uint8_t bb2 = static_cast<uint8_t>((w >> 40) & 0x03U);
    if (ba2 + bb2 < 4) {
        w |= 1ULL << 42;
    } else {
        w |= 0x07ULL << 45;
    }
    return w;
}

uint64_t PackTWord(const Rgb3& c1, const Rgb3& c2, uint8_t dist_idx, uint16_t msb, uint16_t lsb) noexcept {
    uint8_t r1a = (c1[0] >> 2) & 0x03U;
    uint8_t r1b = c1[0] & 0x03U;
    uint64_t w = (static_cast<uint64_t>(r1a) << 59) |
                 (static_cast<uint64_t>(r1b) << 56) |
                 (static_cast<uint64_t>(c1[1]) << 52) |
                 (static_cast<uint64_t>(c1[2]) << 48) |
                 (static_cast<uint64_t>(c2[0]) << 44) |
                 (static_cast<uint64_t>(c2[1]) << 40) |
                 (static_cast<uint64_t>(c2[2]) << 36) |
                 (static_cast<uint64_t>((dist_idx >> 1) & 0x03U) << 34) |
                 (1ULL << 33) |
                 (static_cast<uint64_t>(dist_idx & 0x01U) << 32) |
                 (static_cast<uint64_t>(msb) << 16) |
                 static_cast<uint64_t>(lsb);
    if (r1a + r1b < 4) {
        w |= 1ULL << 58;
    } else {
        w |= 0x07ULL << 61;
    }
    return w;
}

uint64_t PackHWord(const Rgb3& c1, const Rgb3& c2, uint8_t dist_idx, uint16_t msb, uint16_t lsb) noexcept {
    uint8_t g1a = (c1[1] >> 1) & 0x07U;
    uint8_t g1b = c1[1] & 0x01U;
    uint8_t b1a = (c1[2] >> 3) & 0x01U;
    uint8_t b1b = c1[2] & 0x07U;

    uint64_t w = (static_cast<uint64_t>(c1[0]) << 59) |
                 (static_cast<uint64_t>(g1a) << 56) |
                 (static_cast<uint64_t>(g1b) << 52) |
                 (static_cast<uint64_t>(b1a) << 51) |
                 (static_cast<uint64_t>(b1b) << 47) |
                 (static_cast<uint64_t>(c2[0]) << 43) |
                 (static_cast<uint64_t>(c2[1]) << 39) |
                 (static_cast<uint64_t>(c2[2]) << 35) |
                 (static_cast<uint64_t>((dist_idx >> 2) & 0x01U) << 34) |
                 (1ULL << 33) |
                 (static_cast<uint64_t>((dist_idx >> 1) & 0x01U) << 32) |
                 (static_cast<uint64_t>(msb) << 16) |
                 static_cast<uint64_t>(lsb);

    if (g1a & 0x04U) w |= 1ULL << 63;
    uint8_t ga2 = static_cast<uint8_t>((g1b << 1) | b1a);
    uint8_t gb2 = (b1b >> 1) & 0x03U;
    if (ga2 + gb2 < 4) {
        w |= 1ULL << 50;
    } else {
        w |= 0x07ULL << 53;
    }
    return w;
}

struct SubblockFit {
    uint32_t cw = 0;
    uint16_t msb = 0;
    uint16_t lsb = 0;
    uint32_t err = std::numeric_limits<uint32_t>::max();
};

SubblockFit FitSubblock(
    const RgbBlock& rgb,
    const uint8_t indices[8],
    const Rgb3& base,
    uint16_t visible_mask,
    uint32_t err_limit = std::numeric_limits<uint32_t>::max()) noexcept {
    SubblockFit best;
    best.err = err_limit;
    const int32_t br = base[0];
    const int32_t bg = base[1];
    const int32_t bb = base[2];

    for (uint32_t cw = 0; cw < 8; ++cw) {
        const int32_t small = kEtc1ModTable[cw][0];
        const int32_t large = kEtc1ModTable[cw][1];
        const int32_t deltas[4] = {small, large, -small, -large};
        Rgb3 cands[4];
        for (int k = 0; k < 4; ++k) {
            cands[k] = {ClampU8(br + deltas[k]), ClampU8(bg + deltas[k]), ClampU8(bb + deltas[k])};
        }

        uint16_t msb_bits = 0;
        uint16_t lsb_bits = 0;
        uint32_t err = 0;
        for (int i = 0; i < 8; ++i) {
            uint8_t p = indices[i];
            if (!((visible_mask >> p) & 1U)) continue;
            const Rgb3& px = rgb[p];
            uint32_t e0 = SqDiff3(px, cands[0]);
            uint32_t e1 = SqDiff3(px, cands[1]);
            uint32_t e2 = SqDiff3(px, cands[2]);
            uint32_t e3 = SqDiff3(px, cands[3]);

            uint32_t best_code = 0;
            uint32_t best_e = e0;
            if (e1 < best_e) { best_e = e1; best_code = 1; }
            if (e2 < best_e) { best_e = e2; best_code = 2; }
            if (e3 < best_e) { best_e = e3; best_code = 3; }

            err += best_e;
            if (err >= best.err) break;
            if (best_code & 2U) msb_bits |= static_cast<uint16_t>(1U << p);
            if (best_code & 1U) lsb_bits |= static_cast<uint16_t>(1U << p);
        }

        if (err < best.err) {
            best.err = err;
            best.cw = cw;
            best.msb = msb_bits;
            best.lsb = lsb_bits;
            if (best.err == 0) break;
        }
    }
    return best;
}

// Computes signed channel residual (target - reconstructed) over visible pixels of a subblock.
void SubblockResidual(
    const RgbBlock& rgb,
    const uint8_t indices[8],
    const Rgb3& base,
    const SubblockFit& fit,
    uint16_t visible_mask,
    int32_t out_res[3],
    int32_t& out_count) noexcept {
    const int32_t s = kEtc1ModTable[fit.cw][0];
    const int32_t l = kEtc1ModTable[fit.cw][1];
    out_res[0] = out_res[1] = out_res[2] = 0;
    out_count = 0;
    for (int i = 0; i < 8; ++i) {
        uint8_t p = indices[i];
        if (!((visible_mask >> p) & 1U)) continue;
        bool msb = ((fit.msb >> p) & 1U) != 0;
        bool lsb = ((fit.lsb >> p) & 1U) != 0;
        int32_t mag = lsb ? l : s;
        int32_t delta = msb ? -mag : mag;
        out_res[0] += static_cast<int32_t>(rgb[p][0]) - static_cast<int32_t>(ClampU8(base[0] + delta));
        out_res[1] += static_cast<int32_t>(rgb[p][1]) - static_cast<int32_t>(ClampU8(base[1] + delta));
        out_res[2] += static_cast<int32_t>(rgb[p][2]) - static_cast<int32_t>(ClampU8(base[2] + delta));
        ++out_count;
    }
}

uint64_t EncodeConstantRgbBlock(const Rgb3& c) noexcept {
    uint8_t ro = Quantize6(c[0]);
    uint8_t go = Quantize7(c[1]);
    uint8_t bo = Quantize6(c[2]);
    uint64_t best_word = PackPlanarWord(ro, go, bo, ro, go, bo, ro, go, bo);
    uint32_t best_err = 16U * SqDiff3(c, {Extend6To8(ro), Extend7To8(go), Extend6To8(bo)});
    if (best_err == 0) return best_word;

    auto fit_uniform = [&](const Rgb3& base, uint32_t& out_cw, uint32_t& out_code) -> uint32_t {
        uint32_t best_px_err = std::numeric_limits<uint32_t>::max();
        out_cw = 0;
        out_code = 0;
        for (uint32_t cw = 0; cw < 8; ++cw) {
            int32_t s = kEtc1ModTable[cw][0];
            int32_t l = kEtc1ModTable[cw][1];
            int32_t ds[4] = {s, l, -s, -l};
            for (uint32_t code = 0; code < 4; ++code) {
                Rgb3 cand = {
                    ClampU8(static_cast<int32_t>(base[0]) + ds[code]),
                    ClampU8(static_cast<int32_t>(base[1]) + ds[code]),
                    ClampU8(static_cast<int32_t>(base[2]) + ds[code]),
                };
                uint32_t e = SqDiff3(c, cand);
                if (e < best_px_err) {
                    best_px_err = e;
                    out_cw = cw;
                    out_code = code;
                    if (e == 0) return 0;
                }
            }
        }
        return best_px_err;
    };

    uint8_t r5 = Quantize5(c[0]), g5 = Quantize5(c[1]), b5 = Quantize5(c[2]);
    uint32_t cw_d = 0, code_d = 0;
    uint32_t diff_err = 16U * fit_uniform({Extend5To8(r5), Extend5To8(g5), Extend5To8(b5)}, cw_d, code_d);
    if (diff_err < best_err) {
        uint64_t msb = (code_d & 2U) ? 0xffffULL : 0ULL;
        uint64_t lsb = (code_d & 1U) ? 0xffffULL : 0ULL;
        best_err = diff_err;
        best_word = (static_cast<uint64_t>(r5) << 59) |
                    (static_cast<uint64_t>(g5) << 51) |
                    (static_cast<uint64_t>(b5) << 43) |
                    (static_cast<uint64_t>(cw_d) << 37) |
                    (static_cast<uint64_t>(cw_d) << 34) |
                    (1ULL << 33) |
                    (msb << 16) |
                    lsb;
        if (best_err == 0) return best_word;
    }

    uint8_t r4 = Quantize4(c[0]), g4 = Quantize4(c[1]), b4 = Quantize4(c[2]);
    uint32_t cw_i = 0, code_i = 0;
    uint32_t indiv_err = 16U * fit_uniform({Extend4To8(r4), Extend4To8(g4), Extend4To8(b4)}, cw_i, code_i);
    if (indiv_err < best_err) {
        uint64_t msb = (code_i & 2U) ? 0xffffULL : 0ULL;
        uint64_t lsb = (code_i & 1U) ? 0xffffULL : 0ULL;
        best_word = (static_cast<uint64_t>(r4) << 60) |
                    (static_cast<uint64_t>(r4) << 56) |
                    (static_cast<uint64_t>(g4) << 52) |
                    (static_cast<uint64_t>(g4) << 48) |
                    (static_cast<uint64_t>(b4) << 44) |
                    (static_cast<uint64_t>(b4) << 40) |
                    (static_cast<uint64_t>(cw_i) << 37) |
                    (static_cast<uint64_t>(cw_i) << 34) |
                    (msb << 16) |
                    lsb;
    }
    return best_word;
}

void TryEncodeSubblockModes(
    const RgbBlock& rgb,
    bool flip,
    uint16_t visible_mask,
    uint64_t& best_word,
    uint32_t& best_err) noexcept {
    static constexpr uint8_t kSub0NoFlip[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    static constexpr uint8_t kSub1NoFlip[8] = {8, 9, 10, 11, 12, 13, 14, 15};
    static constexpr uint8_t kSub0Flip[8] = {0, 1, 4, 5, 8, 9, 12, 13};
    static constexpr uint8_t kSub1Flip[8] = {2, 3, 6, 7, 10, 11, 14, 15};

    const uint8_t* sub0_idx = flip ? kSub0Flip : kSub0NoFlip;
    const uint8_t* sub1_idx = flip ? kSub1Flip : kSub1NoFlip;

    Rgb3 avg0 = AverageRgb8(rgb, sub0_idx, visible_mask);
    Rgb3 avg1 = AverageRgb8(rgb, sub1_idx, visible_mask);

    // 1. Differential mode (5-bit base + 3-bit signed delta).
    {
        uint8_t r0 = Quantize5(avg0[0]);
        uint8_t g0 = Quantize5(avg0[1]);
        uint8_t b0 = Quantize5(avg0[2]);
        int32_t dr = std::clamp(static_cast<int32_t>(Quantize5(avg1[0])) - static_cast<int32_t>(r0), -4, 3);
        int32_t dg = std::clamp(static_cast<int32_t>(Quantize5(avg1[1])) - static_cast<int32_t>(g0), -4, 3);
        int32_t db = std::clamp(static_cast<int32_t>(Quantize5(avg1[2])) - static_cast<int32_t>(b0), -4, 3);

        uint8_t r1 = static_cast<uint8_t>(static_cast<int32_t>(r0) + dr);
        uint8_t g1 = static_cast<uint8_t>(static_cast<int32_t>(g0) + dg);
        uint8_t b1 = static_cast<uint8_t>(static_cast<int32_t>(b0) + db);
        Rgb3 q0 = {Extend5To8(r0), Extend5To8(g0), Extend5To8(b0)};
        Rgb3 q1 = {Extend5To8(r1), Extend5To8(g1), Extend5To8(b1)};

        SubblockFit f0 = FitSubblock(rgb, sub0_idx, q0, visible_mask, best_err);
        if (f0.err < best_err) {
            SubblockFit f1 = FitSubblock(rgb, sub1_idx, q1, visible_mask, best_err - f0.err);
            if (f1.err < best_err - f0.err) {
                // Residual-guided 1-step refinement independently per subblock (respecting 3-bit delta).
                auto refine_sub = [&](const uint8_t* idxs, uint8_t& qr, uint8_t& qg, uint8_t& qb,
                                      int32_t lo_r, int32_t hi_r, int32_t lo_g, int32_t hi_g,
                                      int32_t lo_b, int32_t hi_b, SubblockFit& fit) {
                    if (fit.err <= 32) return;
                    int32_t res[3], cnt = 0;
                    Rgb3 cur = {Extend5To8(qr), Extend5To8(qg), Extend5To8(qb)};
                    SubblockResidual(rgb, idxs, cur, fit, visible_mask, res, cnt);
                    if (cnt == 0) return;
                    // Subtract mean residual across RGB so we nudge chromaticity, not gray luminance.
                    int32_t lum = (res[0] + res[1] + res[2]) / 3;
                    int32_t thresh = cnt * 3;
                    auto step = [&](int32_t q, int32_t r, int32_t lo, int32_t hi) -> uint8_t {
                        if (r > thresh && q < hi) return static_cast<uint8_t>(q + 1);
                        if (r < -thresh && q > lo) return static_cast<uint8_t>(q - 1);
                        return static_cast<uint8_t>(q);
                    };
                    uint8_t nr = step(qr, res[0] - lum, lo_r, hi_r);
                    uint8_t ng = step(qg, res[1] - lum, lo_g, hi_g);
                    uint8_t nb = step(qb, res[2] - lum, lo_b, hi_b);
                    if (nr == qr && ng == qg && nb == qb) {
                        nr = step(qr, res[0], lo_r, hi_r);
                        ng = step(qg, res[1], lo_g, hi_g);
                        nb = step(qb, res[2], lo_b, hi_b);
                    }
                    if (nr == qr && ng == qg && nb == qb) return;
                    SubblockFit cand = FitSubblock(rgb, idxs, {Extend5To8(nr), Extend5To8(ng), Extend5To8(nb)},
                                                   visible_mask, fit.err);
                    if (cand.err < fit.err) {
                        qr = nr; qg = ng; qb = nb;
                        fit = cand;
                    }
                };
                refine_sub(sub0_idx, r0, g0, b0,
                           std::max<int32_t>(0, static_cast<int32_t>(r1) - 3), std::min<int32_t>(31, static_cast<int32_t>(r1) + 4),
                           std::max<int32_t>(0, static_cast<int32_t>(g1) - 3), std::min<int32_t>(31, static_cast<int32_t>(g1) + 4),
                           std::max<int32_t>(0, static_cast<int32_t>(b1) - 3), std::min<int32_t>(31, static_cast<int32_t>(b1) + 4),
                           f0);
                refine_sub(sub1_idx, r1, g1, b1,
                           std::max<int32_t>(0, static_cast<int32_t>(r0) - 4), std::min<int32_t>(31, static_cast<int32_t>(r0) + 3),
                           std::max<int32_t>(0, static_cast<int32_t>(g0) - 4), std::min<int32_t>(31, static_cast<int32_t>(g0) + 3),
                           std::max<int32_t>(0, static_cast<int32_t>(b0) - 4), std::min<int32_t>(31, static_cast<int32_t>(b0) + 3),
                           f1);

                dr = static_cast<int32_t>(r1) - static_cast<int32_t>(r0);
                dg = static_cast<int32_t>(g1) - static_cast<int32_t>(g0);
                db = static_cast<int32_t>(b1) - static_cast<int32_t>(b0);
                best_err = f0.err + f1.err;
                uint64_t hdr = (static_cast<uint64_t>(r0) << 59) |
                               (static_cast<uint64_t>(static_cast<uint8_t>(dr) & 0x07U) << 56) |
                               (static_cast<uint64_t>(g0) << 51) |
                               (static_cast<uint64_t>(static_cast<uint8_t>(dg) & 0x07U) << 48) |
                               (static_cast<uint64_t>(b0) << 43) |
                               (static_cast<uint64_t>(static_cast<uint8_t>(db) & 0x07U) << 40) |
                               (1ULL << 33) |
                               (static_cast<uint64_t>(flip ? 1U : 0U) << 32);
                best_word = hdr |
                            (static_cast<uint64_t>(f0.cw) << 37) |
                            (static_cast<uint64_t>(f1.cw) << 34) |
                            (static_cast<uint64_t>(f0.msb | f1.msb) << 16) |
                            static_cast<uint64_t>(f0.lsb | f1.lsb);
                if (best_err == 0) return;
            }
        }
    }

    // 2. Individual mode (4-bit independent bases — shines when subblock delta overflows 3 bits).
    {
        uint8_t r0 = Quantize4(avg0[0]);
        uint8_t g0 = Quantize4(avg0[1]);
        uint8_t b0 = Quantize4(avg0[2]);
        uint8_t r1 = Quantize4(avg1[0]);
        uint8_t g1 = Quantize4(avg1[1]);
        uint8_t b1 = Quantize4(avg1[2]);

        SubblockFit f0 = FitSubblock(rgb, sub0_idx, {Extend4To8(r0), Extend4To8(g0), Extend4To8(b0)}, visible_mask, best_err);
        if (f0.err >= best_err) return;
        SubblockFit f1 = FitSubblock(rgb, sub1_idx, {Extend4To8(r1), Extend4To8(g1), Extend4To8(b1)}, visible_mask, best_err - f0.err);
        if (f1.err >= best_err - f0.err) return;

        auto refine_indiv = [&](const uint8_t* idxs, uint8_t& qr, uint8_t& qg, uint8_t& qb, SubblockFit& fit) {
            if (fit.err <= 48) return;
            int32_t res[3], cnt = 0;
            SubblockResidual(rgb, idxs, {Extend4To8(qr), Extend4To8(qg), Extend4To8(qb)}, fit, visible_mask, res, cnt);
            if (cnt == 0) return;
            int32_t lum = (res[0] + res[1] + res[2]) / 3;
            int32_t thresh = cnt * 6;
            auto step4 = [&](uint8_t q, int32_t r) -> uint8_t {
                if (r > thresh && q < 15) return static_cast<uint8_t>(q + 1);
                if (r < -thresh && q > 0) return static_cast<uint8_t>(q - 1);
                return q;
            };
            uint8_t nr = step4(qr, res[0] - lum);
            uint8_t ng = step4(qg, res[1] - lum);
            uint8_t nb = step4(qb, res[2] - lum);
            if (nr == qr && ng == qg && nb == qb) return;
            SubblockFit cand = FitSubblock(rgb, idxs, {Extend4To8(nr), Extend4To8(ng), Extend4To8(nb)}, visible_mask, fit.err);
            if (cand.err < fit.err) {
                qr = nr; qg = ng; qb = nb;
                fit = cand;
            }
        };
        refine_indiv(sub0_idx, r0, g0, b0, f0);
        refine_indiv(sub1_idx, r1, g1, b1, f1);

        if (f0.err + f1.err < best_err) {
            best_err = f0.err + f1.err;
            uint64_t hdr = (static_cast<uint64_t>(r0) << 60) |
                           (static_cast<uint64_t>(r1) << 56) |
                           (static_cast<uint64_t>(g0) << 52) |
                           (static_cast<uint64_t>(g1) << 48) |
                           (static_cast<uint64_t>(b0) << 44) |
                           (static_cast<uint64_t>(b1) << 40) |
                           (static_cast<uint64_t>(flip ? 1U : 0U) << 32);
            best_word = hdr |
                        (static_cast<uint64_t>(f0.cw) << 37) |
                        (static_cast<uint64_t>(f1.cw) << 34) |
                        (static_cast<uint64_t>(f0.msb | f1.msb) << 16) |
                        static_cast<uint64_t>(f0.lsb | f1.lsb);
        }
    }
}

std::pair<uint64_t, uint32_t> TryEncodePlanar(const RgbBlock& rgb, uint16_t visible_mask) noexcept {
    float co_f[3] = {0, 0, 0};
    float ch_f[3] = {0, 0, 0};
    float cv_f[3] = {0, 0, 0};

    bool solved_masked = false;
    if (visible_mask != 0xFFFFU) {
        // Weighted least-squares plane fit over visible pixels only.
        float sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
        for (int x = 0; x < 4; ++x) {
            for (int y = 0; y < 4; ++y) {
                int p = x * 4 + y;
                if (!((visible_mask >> p) & 1U)) continue;
                float fx = static_cast<float>(x);
                float fy = static_cast<float>(y);
                sw += 1.0f;
                sx += fx;
                sy += fy;
                sxx += fx * fx;
                sxy += fx * fy;
                syy += fy * fy;
            }
        }
        float det = sw * (sxx * syy - sxy * sxy) -
                    sx * (sx * syy - sxy * sy) +
                    sy * (sx * sxy - sxx * sy);
        if (std::fabs(det) > 1e-3f) {
            float inv_det = 1.0f / det;
            float m00 = (sxx * syy - sxy * sxy) * inv_det;
            float m01 = -(sx * syy - sxy * sy) * inv_det;
            float m02 = (sx * sxy - sxx * sy) * inv_det;
            float m11 = (sw * syy - sy * sy) * inv_det;
            float m12 = -(sw * sxy - sx * sy) * inv_det;
            float m22 = (sw * sxx - sx * sx) * inv_det;
            for (int ch = 0; ch < 3; ++ch) {
                float sc = 0, sxc = 0, syc = 0;
                for (int x = 0; x < 4; ++x) {
                    for (int y = 0; y < 4; ++y) {
                        int p = x * 4 + y;
                        if (!((visible_mask >> p) & 1U)) continue;
                        float c = static_cast<float>(rgb[p][ch]);
                        sc += c;
                        sxc += static_cast<float>(x) * c;
                        syc += static_cast<float>(y) * c;
                    }
                }
                float c0 = m00 * sc + m01 * sxc + m02 * syc;
                float gx = m01 * sc + m11 * sxc + m12 * syc;
                float gy = m02 * sc + m12 * sxc + m22 * syc;
                co_f[ch] = std::clamp(c0, 0.0f, 255.0f);
                ch_f[ch] = std::clamp(c0 + 4.0f * gx, 0.0f, 255.0f);
                cv_f[ch] = std::clamp(c0 + 4.0f * gy, 0.0f, 255.0f);
            }
            solved_masked = true;
        }
    }

    if (!solved_masked) {
        for (int ch = 0; ch < 3; ++ch) {
            int32_t sum_c = 0;
            int32_t sum_2xc = 0;
            int32_t sum_2yc = 0;
            for (int x = 0; x < 4; ++x) {
                int32_t wx = x * 2 - 3;
                int32_t col_sum = 0;
                for (int y = 0; y < 4; ++y) {
                    int32_t c = rgb[x * 4 + y][ch];
                    col_sum += c;
                    sum_2yc += (y * 2 - 3) * c;
                }
                sum_c += col_sum;
                sum_2xc += wx * col_sum;
            }
            float mean = static_cast<float>(sum_c) / 16.0f;
            float sx = (static_cast<float>(sum_2xc) * 0.5f) / 20.0f;
            float sy = (static_cast<float>(sum_2yc) * 0.5f) / 20.0f;
            float c0 = mean - 1.5f * sx - 1.5f * sy;
            co_f[ch] = std::clamp(c0, 0.0f, 255.0f);
            ch_f[ch] = std::clamp(c0 + 4.0f * sx, 0.0f, 255.0f);
            cv_f[ch] = std::clamp(c0 + 4.0f * sy, 0.0f, 255.0f);
        }
    }

    uint8_t ro = Quantize6(static_cast<uint8_t>(co_f[0] + 0.5f));
    uint8_t go = Quantize7(static_cast<uint8_t>(co_f[1] + 0.5f));
    uint8_t bo = Quantize6(static_cast<uint8_t>(co_f[2] + 0.5f));
    uint8_t rh = Quantize6(static_cast<uint8_t>(ch_f[0] + 0.5f));
    uint8_t gh = Quantize7(static_cast<uint8_t>(ch_f[1] + 0.5f));
    uint8_t bh = Quantize6(static_cast<uint8_t>(ch_f[2] + 0.5f));
    uint8_t rv = Quantize6(static_cast<uint8_t>(cv_f[0] + 0.5f));
    uint8_t gv = Quantize7(static_cast<uint8_t>(cv_f[1] + 0.5f));
    uint8_t bv = Quantize6(static_cast<uint8_t>(cv_f[2] + 0.5f));

    uint64_t word = PackPlanarWord(ro, go, bo, rh, gh, bh, rv, gv, bv);
    int32_t er0 = Extend6To8(ro);
    int32_t eg0 = Extend7To8(go);
    int32_t eb0 = Extend6To8(bo);
    int32_t dx_r = static_cast<int32_t>(Extend6To8(rh)) - er0;
    int32_t dx_g = static_cast<int32_t>(Extend7To8(gh)) - eg0;
    int32_t dx_b = static_cast<int32_t>(Extend6To8(bh)) - eb0;
    int32_t dy_r = static_cast<int32_t>(Extend6To8(rv)) - er0;
    int32_t dy_g = static_cast<int32_t>(Extend7To8(gv)) - eg0;
    int32_t dy_b = static_cast<int32_t>(Extend6To8(bv)) - eb0;

    uint32_t err = 0;
    for (int32_t x = 0; x < 4; ++x) {
        int32_t bx_r = x * dx_r + 4 * er0 + 2;
        int32_t bx_g = x * dx_g + 4 * eg0 + 2;
        int32_t bx_b = x * dx_b + 4 * eb0 + 2;
        for (int32_t y = 0; y < 4; ++y) {
            int p = x * 4 + y;
            if (!((visible_mask >> p) & 1U)) continue;
            uint8_t r = ClampU8((bx_r + y * dy_r) >> 2);
            uint8_t g = ClampU8((bx_g + y * dy_g) >> 2);
            uint8_t b = ClampU8((bx_b + y * dy_b) >> 2);
            err += SqDiff3(rgb[p], {r, g, b});
        }
    }
    return {word, err};
}

bool MatchPalette4(
    const RgbBlock& rgb,
    const Rgb3 pal[4],
    uint32_t limit,
    uint16_t visible_mask,
    uint16_t& out_msb,
    uint16_t& out_lsb,
    uint32_t& out_err) noexcept {
    uint32_t err = 0;
    uint16_t msb = 0;
    uint16_t lsb = 0;
    for (int p = 0; p < 16; ++p) {
        if (!((visible_mask >> p) & 1U)) continue;
        const Rgb3& px = rgb[p];
        uint32_t e0 = SqDiff3(px, pal[0]);
        uint32_t e1 = SqDiff3(px, pal[1]);
        uint32_t e2 = SqDiff3(px, pal[2]);
        uint32_t e3 = SqDiff3(px, pal[3]);

        uint32_t best_k = 0;
        uint32_t best_e = e0;
        if (e1 < best_e) { best_e = e1; best_k = 1; }
        if (e2 < best_e) { best_e = e2; best_k = 2; }
        if (e3 < best_e) { best_e = e3; best_k = 3; }

        err += best_e;
        if (err >= limit) return false;
        if (best_k & 2U) msb |= static_cast<uint16_t>(1U << p);
        if (best_k & 1U) lsb |= static_cast<uint16_t>(1U << p);
    }
    out_msb = msb;
    out_lsb = lsb;
    out_err = err;
    return true;
}

std::pair<uint64_t, uint32_t> TryEncodeTAndH(
    const RgbBlock& rgb,
    uint32_t current_best,
    uint16_t visible_mask) noexcept {
    uint8_t min_c[3] = {255, 255, 255};
    uint8_t max_c[3] = {0, 0, 0};
    for (int p = 0; p < 16; ++p) {
        if (!((visible_mask >> p) & 1U)) continue;
        for (int c = 0; c < 3; ++c) {
            min_c[c] = std::min(min_c[c], rgb[p][c]);
            max_c[c] = std::max(max_c[c], rgb[p][c]);
        }
    }
    int axis = 0;
    if (max_c[1] - min_c[1] > max_c[axis] - min_c[axis]) axis = 1;
    if (max_c[2] - min_c[2] > max_c[axis] - min_c[axis]) axis = 2;
    uint8_t split = static_cast<uint8_t>((static_cast<uint32_t>(min_c[axis]) + max_c[axis]) / 2U);

    uint32_t sum_lo[3] = {0, 0, 0}, cnt_lo = 0;
    uint32_t sum_hi[3] = {0, 0, 0}, cnt_hi = 0;
    for (int p = 0; p < 16; ++p) {
        if (!((visible_mask >> p) & 1U)) continue;
        if (rgb[p][axis] <= split) {
            ++cnt_lo;
            sum_lo[0] += rgb[p][0]; sum_lo[1] += rgb[p][1]; sum_lo[2] += rgb[p][2];
        } else {
            ++cnt_hi;
            sum_hi[0] += rgb[p][0]; sum_hi[1] += rgb[p][1]; sum_hi[2] += rgb[p][2];
        }
    }
    if (cnt_lo == 0 || cnt_hi == 0) {
        return {0, std::numeric_limits<uint32_t>::max()};
    }

    Rgb3 mean_lo = {
        static_cast<uint8_t>((sum_lo[0] + cnt_lo / 2) / cnt_lo),
        static_cast<uint8_t>((sum_lo[1] + cnt_lo / 2) / cnt_lo),
        static_cast<uint8_t>((sum_lo[2] + cnt_lo / 2) / cnt_lo),
    };
    Rgb3 mean_hi = {
        static_cast<uint8_t>((sum_hi[0] + cnt_hi / 2) / cnt_hi),
        static_cast<uint8_t>((sum_hi[1] + cnt_hi / 2) / cnt_hi),
        static_cast<uint8_t>((sum_hi[2] + cnt_hi / 2) / cnt_hi),
    };

    // 1 refinement pass of 2-means clustering in full 3D RGB space.
    uint32_t r_lo[3] = {0, 0, 0}, rc_lo = 0;
    uint32_t r_hi[3] = {0, 0, 0}, rc_hi = 0;
    for (int p = 0; p < 16; ++p) {
        if (!((visible_mask >> p) & 1U)) continue;
        if (SqDiff3(rgb[p], mean_lo) <= SqDiff3(rgb[p], mean_hi)) {
            ++rc_lo;
            r_lo[0] += rgb[p][0]; r_lo[1] += rgb[p][1]; r_lo[2] += rgb[p][2];
        } else {
            ++rc_hi;
            r_hi[0] += rgb[p][0]; r_hi[1] += rgb[p][1]; r_hi[2] += rgb[p][2];
        }
    }
    if (rc_lo > 0 && rc_hi > 0) {
        mean_lo = {
            static_cast<uint8_t>((r_lo[0] + rc_lo / 2) / rc_lo),
            static_cast<uint8_t>((r_lo[1] + rc_lo / 2) / rc_lo),
            static_cast<uint8_t>((r_lo[2] + rc_lo / 2) / rc_lo),
        };
        mean_hi = {
            static_cast<uint8_t>((r_hi[0] + rc_hi / 2) / rc_hi),
            static_cast<uint8_t>((r_hi[1] + rc_hi / 2) / rc_hi),
            static_cast<uint8_t>((r_hi[2] + rc_hi / 2) / rc_hi),
        };
    }

    Rgb3 c1_4 = {Quantize4(mean_lo[0]), Quantize4(mean_lo[1]), Quantize4(mean_lo[2])};
    Rgb3 c2_4 = {Quantize4(mean_hi[0]), Quantize4(mean_hi[1]), Quantize4(mean_hi[2])};

    uint32_t best_err = current_best;
    uint64_t best_word = 0;

    // T-Mode (both cluster orderings).
    const std::pair<Rgb3, Rgb3> orders[2] = {{c1_4, c2_4}, {c2_4, c1_4}};
    for (const auto& [a4, b4] : orders) {
        Rgb3 base1 = {Extend4To8(a4[0]), Extend4To8(a4[1]), Extend4To8(a4[2])};
        Rgb3 base2 = {Extend4To8(b4[0]), Extend4To8(b4[1]), Extend4To8(b4[2])};
        for (uint8_t di = 0; di < 8; ++di) {
            int32_t d = kEtc2ThDist[di];
            Rgb3 pal[4] = {
                base1,
                {ClampU8(base2[0] + d), ClampU8(base2[1] + d), ClampU8(base2[2] + d)},
                base2,
                {ClampU8(base2[0] - d), ClampU8(base2[1] - d), ClampU8(base2[2] - d)},
            };
            uint16_t msb = 0, lsb = 0;
            uint32_t err = 0;
            if (MatchPalette4(rgb, pal, best_err, visible_mask, msb, lsb, err)) {
                best_err = err;
                best_word = PackTWord(a4, b4, di, msb, lsb);
            }
        }
    }

    // H-Mode (requires c1_4 != c2_4).
    if (c1_4 != c2_4) {
        uint32_t val1 = (static_cast<uint32_t>(c1_4[0]) << 8) |
                        (static_cast<uint32_t>(c1_4[1]) << 4) | c1_4[2];
        uint32_t val2 = (static_cast<uint32_t>(c2_4[0]) << 8) |
                        (static_cast<uint32_t>(c2_4[1]) << 4) | c2_4[2];
        for (uint8_t di = 0; di < 8; ++di) {
            int32_t d = kEtc2ThDist[di];
            bool want_ge = (di & 1U) == 1U;
            const Rgb3& a4 = ((val1 >= val2) == want_ge) ? c1_4 : c2_4;
            const Rgb3& b4 = ((val1 >= val2) == want_ge) ? c2_4 : c1_4;
            Rgb3 base1 = {Extend4To8(a4[0]), Extend4To8(a4[1]), Extend4To8(a4[2])};
            Rgb3 base2 = {Extend4To8(b4[0]), Extend4To8(b4[1]), Extend4To8(b4[2])};
            Rgb3 pal[4] = {
                {ClampU8(base1[0] + d), ClampU8(base1[1] + d), ClampU8(base1[2] + d)},
                {ClampU8(base1[0] - d), ClampU8(base1[1] - d), ClampU8(base1[2] - d)},
                {ClampU8(base2[0] + d), ClampU8(base2[1] + d), ClampU8(base2[2] + d)},
                {ClampU8(base2[0] - d), ClampU8(base2[1] - d), ClampU8(base2[2] - d)},
            };
            uint16_t msb = 0, lsb = 0;
            uint32_t err = 0;
            if (MatchPalette4(rgb, pal, best_err, visible_mask, msb, lsb, err)) {
                best_err = err;
                best_word = PackHWord(a4, b4, di, msb, lsb);
            }
        }
    }

    return {best_word, best_err};
}

RgbBlock DecodeSubblocks(uint64_t word, bool flip, const Rgb3& base1, const Rgb3& base2) noexcept {
    size_t cw1 = static_cast<size_t>((word >> 37) & 0x07U);
    size_t cw2 = static_cast<size_t>((word >> 34) & 0x07U);
    RgbBlock out{};
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            int p = x * 4 + y;
            bool second = flip ? (y >= 2) : (x >= 2);
            const Rgb3& base = second ? base2 : base1;
            size_t cw = second ? cw2 : cw1;
            bool msb = ((word >> (16 + p)) & 1ULL) != 0;
            bool lsb = ((word >> p) & 1ULL) != 0;
            int32_t mag = kEtc1ModTable[cw][lsb ? 1 : 0];
            int32_t delta = msb ? -mag : mag;
            out[p] = {ClampU8(base[0] + delta), ClampU8(base[1] + delta), ClampU8(base[2] + delta)};
        }
    }
    return out;
}

RgbBlock DecodeTMode(uint64_t word) noexcept {
    uint8_t r1 = static_cast<uint8_t>((((word >> 59) & 0x03U) << 2) | ((word >> 56) & 0x03U));
    uint8_t g1 = static_cast<uint8_t>((word >> 52) & 0x0fU);
    uint8_t b1 = static_cast<uint8_t>((word >> 48) & 0x0fU);
    uint8_t r2 = static_cast<uint8_t>((word >> 44) & 0x0fU);
    uint8_t g2 = static_cast<uint8_t>((word >> 40) & 0x0fU);
    uint8_t b2 = static_cast<uint8_t>((word >> 36) & 0x0fU);
    size_t dist_idx = static_cast<size_t>((((word >> 34) & 0x03U) << 1) | ((word >> 32) & 0x01U));
    int32_t d = kEtc2ThDist[dist_idx];

    Rgb3 base1 = {Extend4To8(r1), Extend4To8(g1), Extend4To8(b1)};
    Rgb3 base2 = {Extend4To8(r2), Extend4To8(g2), Extend4To8(b2)};
    Rgb3 pal[4] = {
        base1,
        {ClampU8(base2[0] + d), ClampU8(base2[1] + d), ClampU8(base2[2] + d)},
        base2,
        {ClampU8(base2[0] - d), ClampU8(base2[1] - d), ClampU8(base2[2] - d)},
    };
    RgbBlock out{};
    for (int p = 0; p < 16; ++p) {
        size_t idx = static_cast<size_t>((((word >> (16 + p)) & 1ULL) << 1) | ((word >> p) & 1ULL));
        out[p] = pal[idx];
    }
    return out;
}

RgbBlock DecodeHMode(uint64_t word) noexcept {
    uint8_t r1 = static_cast<uint8_t>((word >> 59) & 0x0fU);
    uint8_t g1 = static_cast<uint8_t>((((word >> 56) & 0x07U) << 1) | ((word >> 52) & 0x01U));
    uint8_t b1 = static_cast<uint8_t>((((word >> 51) & 0x01U) << 3) | ((word >> 47) & 0x07U));
    uint8_t r2 = static_cast<uint8_t>((word >> 43) & 0x0fU);
    uint8_t g2 = static_cast<uint8_t>((word >> 39) & 0x0fU);
    uint8_t b2 = static_cast<uint8_t>((word >> 35) & 0x0fU);

    Rgb3 base1 = {Extend4To8(r1), Extend4To8(g1), Extend4To8(b1)};
    Rgb3 base2 = {Extend4To8(r2), Extend4To8(g2), Extend4To8(b2)};
    uint32_t val1 = (static_cast<uint32_t>(base1[0]) << 16) |
                    (static_cast<uint32_t>(base1[1]) << 8) | base1[2];
    uint32_t val2 = (static_cast<uint32_t>(base2[0]) << 16) |
                    (static_cast<uint32_t>(base2[1]) << 8) | base2[2];
    uint64_t dc = (val1 >= val2) ? 1ULL : 0ULL;
    size_t dist_idx = static_cast<size_t>((((word >> 34) & 1ULL) << 2) |
                                          (((word >> 32) & 1ULL) << 1) | dc);
    int32_t d = kEtc2ThDist[dist_idx];
    Rgb3 pal[4] = {
        {ClampU8(base1[0] + d), ClampU8(base1[1] + d), ClampU8(base1[2] + d)},
        {ClampU8(base1[0] - d), ClampU8(base1[1] - d), ClampU8(base1[2] - d)},
        {ClampU8(base2[0] + d), ClampU8(base2[1] + d), ClampU8(base2[2] + d)},
        {ClampU8(base2[0] - d), ClampU8(base2[1] - d), ClampU8(base2[2] - d)},
    };
    RgbBlock out{};
    for (int p = 0; p < 16; ++p) {
        size_t idx = static_cast<size_t>((((word >> (16 + p)) & 1ULL) << 1) | ((word >> p) & 1ULL));
        out[p] = pal[idx];
    }
    return out;
}

RgbBlock DecodePlanarMode(uint64_t word) noexcept {
    uint8_t ro6 = static_cast<uint8_t>((word >> 57) & 0x3fU);
    uint8_t go7 = static_cast<uint8_t>((((word >> 56) & 0x01U) << 6) | ((word >> 49) & 0x3fU));
    uint8_t bo6 = static_cast<uint8_t>((((word >> 48) & 0x01U) << 5) |
                                       (((word >> 43) & 0x03U) << 3) |
                                       ((word >> 39) & 0x07U));
    uint8_t rh6 = static_cast<uint8_t>((((word >> 34) & 0x1fU) << 1) | ((word >> 32) & 0x01U));
    uint8_t gh7 = static_cast<uint8_t>((word >> 25) & 0x7fU);
    uint8_t bh6 = static_cast<uint8_t>((word >> 19) & 0x3fU);
    uint8_t rv6 = static_cast<uint8_t>((word >> 13) & 0x3fU);
    uint8_t gv7 = static_cast<uint8_t>((word >> 6) & 0x7fU);
    uint8_t bv6 = static_cast<uint8_t>(word & 0x3fU);

    int32_t ro = Extend6To8(ro6), go = Extend7To8(go7), bo = Extend6To8(bo6);
    int32_t rh = Extend6To8(rh6), gh = Extend7To8(gh7), bh = Extend6To8(bh6);
    int32_t rv = Extend6To8(rv6), gv = Extend7To8(gv7), bv = Extend6To8(bv6);

    RgbBlock out{};
    for (int32_t x = 0; x < 4; ++x) {
        for (int32_t y = 0; y < 4; ++y) {
            int32_t r = (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2;
            int32_t g = (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2;
            int32_t b = (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2;
            out[x * 4 + y] = {ClampU8(r), ClampU8(g), ClampU8(b)};
        }
    }
    return out;
}

template <typename Fn>
void ParallelBlockRows(
    size_t bh,
    size_t bw,
    size_t max_threads,
    const volatile int32_t* cancel_flag,
    Fn&& row_fn) {
    constexpr size_t kStripe = 8;
    size_t total_blocks = bw * bh;
    if (max_threads <= 1 || total_blocks < 4096 || bh <= kStripe) {
        for (size_t by = 0; by < bh; ++by) {
            if ((by & 7U) == 0 && IsCancelled(cancel_flag)) {
                throw ConvertError(MIRAGE_MOBILE_CANCELLED, "cancelled");
            }
            row_fn(by);
        }
        return;
    }

    size_t worker_count = std::min(max_threads, (bh + kStripe - 1) / kStripe);
    std::atomic<size_t> next_row{0};
    std::atomic<bool> cancelled{false};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t t = 0; t < worker_count; ++t) {
        workers.emplace_back([&]() {
            while (true) {
                if (IsCancelled(cancel_flag)) {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                size_t start = next_row.fetch_add(kStripe, std::memory_order_relaxed);
                if (start >= bh) return;
                size_t end = std::min(start + kStripe, bh);
                for (size_t by = start; by < end; ++by) {
                    row_fn(by);
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    if (cancelled.load(std::memory_order_relaxed)) {
        throw ConvertError(MIRAGE_MOBILE_CANCELLED, "cancelled");
    }
}

} // namespace

uint64_t EncodeEacAlphaBlock(const std::array<uint8_t, 16>& alpha) noexcept {
    uint8_t min_a = 255;
    uint8_t max_a = 0;
    for (uint8_t a : alpha) {
        if (a < min_a) min_a = a;
        if (a > max_a) max_a = a;
    }
    if (min_a == max_a) {
        return static_cast<uint64_t>(min_a) << 56;
    }

    int32_t span = static_cast<int32_t>(max_a) - static_cast<int32_t>(min_a);
    int32_t mid = (static_cast<int32_t>(min_a) + static_cast<int32_t>(max_a) + 1) / 2;

    uint32_t best_err = std::numeric_limits<uint32_t>::max();
    uint64_t best_word = static_cast<uint64_t>(mid) << 56;

    for (uint32_t tbl_idx = 0; tbl_idx < 16; ++tbl_idx) {
        const int32_t* mods = kEacModTable[tbl_idx];
        int32_t tbl_span = mods[7] - mods[3];
        int32_t approx_mul = std::clamp((span + tbl_span / 2) / tbl_span, 1, 15);
        int32_t mul_lo = std::max(approx_mul - 1, 1);
        int32_t mul_hi = std::min(approx_mul + 1, 15);

        for (int32_t mul = mul_lo; mul <= mul_hi; ++mul) {
            int32_t prev_base = -1;
            static constexpr int32_t kBaseDeltas[3] = {-2, 0, 2};
            for (int32_t bd : kBaseDeltas) {
                int32_t base = std::clamp(mid + bd, 0, 255);
                if (base == prev_base) continue;
                prev_base = base;

                uint8_t palette[8];
                for (int k = 0; k < 8; ++k) {
                    palette[k] = ClampU8(base + mods[k] * mul);
                }

                uint32_t err = 0;
                uint64_t indices = 0;
                for (int p = 0; p < 16; ++p) {
                    uint8_t a = alpha[p];
                    uint32_t best_k = 0;
                    uint32_t best_d = 0;
                    if (a <= palette[0]) {
                        best_k = 0;
                        best_d = static_cast<uint32_t>(palette[0] - a);
                        for (uint32_t k = 1; k < 4; ++k) {
                            uint32_t d = static_cast<uint32_t>(
                                a >= palette[k] ? (a - palette[k]) : (palette[k] - a));
                            if (d < best_d) {
                                best_d = d;
                                best_k = k;
                            }
                        }
                    } else if (a >= palette[4]) {
                        best_k = 4;
                        best_d = static_cast<uint32_t>(a - palette[4]);
                        for (uint32_t k = 5; k < 8; ++k) {
                            uint32_t d = static_cast<uint32_t>(
                                a >= palette[k] ? (a - palette[k]) : (palette[k] - a));
                            if (d < best_d) {
                                best_d = d;
                                best_k = k;
                            }
                        }
                    } else {
                        uint32_t d0 = static_cast<uint32_t>(a - palette[0]);
                        uint32_t d4 = static_cast<uint32_t>(palette[4] - a);
                        if (d0 <= d4) {
                            best_k = 0;
                            best_d = d0;
                        } else {
                            best_k = 4;
                            best_d = d4;
                        }
                    }
                    err += best_d * best_d;
                    if (err >= best_err) break;
                    indices = (indices << 3) | static_cast<uint64_t>(best_k);
                }

                if (err < best_err) {
                    best_err = err;
                    best_word = (static_cast<uint64_t>(base) << 56) |
                                (static_cast<uint64_t>(mul) << 52) |
                                (static_cast<uint64_t>(tbl_idx) << 48) |
                                indices;
                    if (best_err == 0) return best_word;
                }
            }
        }
    }
    return best_word;
}

std::array<uint8_t, 16> DecodeEacAlphaBlock(uint64_t word) noexcept {
    int32_t base = static_cast<int32_t>((word >> 56) & 0xffU);
    int32_t mul = static_cast<int32_t>((word >> 52) & 0x0fU);
    size_t tbl = static_cast<size_t>((word >> 48) & 0x0fU);
    const int32_t* mods = kEacModTable[tbl];

    std::array<uint8_t, 16> out{};
    for (int p = 0; p < 16; ++p) {
        size_t idx = static_cast<size_t>((word >> (45 - p * 3)) & 0x07U);
        out[p] = ClampU8(base + mods[idx] * mul);
    }
    return out;
}

uint64_t EncodeEtc2RgbBlock(const RgbBlock& rgb, uint16_t visible_mask) noexcept {
    if (visible_mask == 0) visible_mask = 0xFFFFU;

    // Fast shortcut if all visible pixels share the exact same color.
    int first_idx = -1;
    bool all_same = true;
    for (int p = 0; p < 16; ++p) {
        if (!((visible_mask >> p) & 1U)) continue;
        if (first_idx < 0) {
            first_idx = p;
        } else if (rgb[p] != rgb[first_idx]) {
            all_same = false;
            break;
        }
    }
    if (all_same && first_idx >= 0) {
        return EncodeConstantRgbBlock(rgb[first_idx]);
    }

    auto [best_word, best_err] = TryEncodePlanar(rgb, visible_mask);
    if (best_err <= 8) return best_word;

    TryEncodeSubblockModes(rgb, false, visible_mask, best_word, best_err);
    if (best_err == 0) return best_word;
    TryEncodeSubblockModes(rgb, true, visible_mask, best_word, best_err);
    if (best_err == 0) return best_word;

    if (best_err > 48) {
        auto [thw, the] = TryEncodeTAndH(rgb, best_err, visible_mask);
        if (the < best_err) {
            best_word = thw;
        }
    }
    return best_word;
}

RgbBlock DecodeEtc2RgbBlock(uint64_t word) noexcept {
    bool diff = ((word >> 33) & 1ULL) != 0;
    bool flip = ((word >> 32) & 1ULL) != 0;

    if (!diff) {
        Rgb3 b1 = {
            Extend4To8(static_cast<uint8_t>((word >> 60) & 0x0fU)),
            Extend4To8(static_cast<uint8_t>((word >> 52) & 0x0fU)),
            Extend4To8(static_cast<uint8_t>((word >> 44) & 0x0fU)),
        };
        Rgb3 b2 = {
            Extend4To8(static_cast<uint8_t>((word >> 56) & 0x0fU)),
            Extend4To8(static_cast<uint8_t>((word >> 48) & 0x0fU)),
            Extend4To8(static_cast<uint8_t>((word >> 40) & 0x0fU)),
        };
        return DecodeSubblocks(word, flip, b1, b2);
    }

    int32_t r = static_cast<int32_t>((word >> 59) & 0x1fU);
    int32_t dr = SignExt3(static_cast<uint8_t>((word >> 56) & 0x07U));
    int32_t g = static_cast<int32_t>((word >> 51) & 0x1fU);
    int32_t dg = SignExt3(static_cast<uint8_t>((word >> 48) & 0x07U));
    int32_t b = static_cast<int32_t>((word >> 43) & 0x1fU);
    int32_t db = SignExt3(static_cast<uint8_t>((word >> 40) & 0x07U));

    int32_t rs = r + dr;
    int32_t gs = g + dg;
    int32_t bs = b + db;

    if (rs < 0 || rs > 31) return DecodeTMode(word);
    if (gs < 0 || gs > 31) return DecodeHMode(word);
    if (bs < 0 || bs > 31) return DecodePlanarMode(word);

    Rgb3 base1 = {
        Extend5To8(static_cast<uint8_t>(r)),
        Extend5To8(static_cast<uint8_t>(g)),
        Extend5To8(static_cast<uint8_t>(b)),
    };
    Rgb3 base2 = {
        Extend5To8(static_cast<uint8_t>(rs)),
        Extend5To8(static_cast<uint8_t>(gs)),
        Extend5To8(static_cast<uint8_t>(bs)),
    };
    return DecodeSubblocks(word, flip, base1, base2);
}

std::vector<uint8_t> EncodeEtc2Rgb8(
    const uint8_t* rgba,
    size_t width,
    size_t height,
    size_t max_threads,
    const volatile int32_t* cancel_flag) {
    size_t bw = (width + 3) / 4;
    size_t bh = (height + 3) / 4;
    std::vector<uint8_t> out(bw * bh * 8);

    ParallelBlockRows(bh, bw, max_threads, cancel_flag, [&](size_t by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            RgbaBlock block = ExtractBlockColMajor(rgba, width, height, bx, by);
            RgbBlock rgb{};
            for (int p = 0; p < 16; ++p) {
                rgb[p] = {block[p][0], block[p][1], block[p][2]};
            }
            uint64_t word = EncodeEtc2RgbBlock(rgb, 0xFFFFU);
            WriteU64BE(out.data() + (by * bw + bx) * 8, word);
        }
    });
    return out;
}

std::vector<uint8_t> EncodeEtc2Rgba8(
    const uint8_t* rgba,
    size_t width,
    size_t height,
    size_t max_threads,
    const volatile int32_t* cancel_flag) {
    size_t bw = (width + 3) / 4;
    size_t bh = (height + 3) / 4;
    std::vector<uint8_t> out(bw * bh * 16);

    ParallelBlockRows(bh, bw, max_threads, cancel_flag, [&](size_t by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            RgbaBlock block = ExtractBlockColMajor(rgba, width, height, bx, by);
            RgbBlock rgb{};
            std::array<uint8_t, 16> alpha{};
            uint16_t visible_mask = 0;
            for (int p = 0; p < 16; ++p) {
                rgb[p] = {block[p][0], block[p][1], block[p][2]};
                alpha[p] = block[p][3];
                if (alpha[p] > 0) {
                    visible_mask |= static_cast<uint16_t>(1U << p);
                }
            }
            uint64_t alpha_word = EncodeEacAlphaBlock(alpha);
            uint64_t rgb_word = EncodeEtc2RgbBlock(rgb, visible_mask == 0 ? 0xFFFFU : visible_mask);
            uint8_t* dst = out.data() + (by * bw + bx) * 16;
            WriteU64BE(dst, alpha_word);
            WriteU64BE(dst + 8, rgb_word);
        }
    });
    return out;
}

std::vector<uint8_t> DecodeEtc2Rgb8(
    const uint8_t* src,
    size_t src_size,
    size_t width,
    size_t height,
    const std::string& label) {
    if (width == 0 || height == 0) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    size_t bw = (width + 3) / 4;
    size_t bh = (height + 3) / 4;
    uint64_t needed = static_cast<uint64_t>(bw) * bh * 8ULL;
    if (!src || src_size < needed) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    std::vector<uint8_t> out(width * height * 4, 255);
    const uint8_t* p = src;
    for (size_t by = 0; by < bh; ++by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            uint64_t word = ReadU64BE(p);
            p += 8;
            RgbBlock rgb = DecodeEtc2RgbBlock(word);
            RgbaBlock block{};
            for (int k = 0; k < 16; ++k) {
                block[k] = {rgb[k][0], rgb[k][1], rgb[k][2], 255};
            }
            WriteBlockColMajor(out.data(), width, height, bx, by, block);
        }
    }
    return out;
}

std::vector<uint8_t> DecodeEtc2Rgba8(
    const uint8_t* src,
    size_t src_size,
    size_t width,
    size_t height,
    const std::string& label) {
    if (width == 0 || height == 0) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    size_t bw = (width + 3) / 4;
    size_t bh = (height + 3) / 4;
    uint64_t needed = static_cast<uint64_t>(bw) * bh * 16ULL;
    if (!src || src_size < needed) {
        throw ConvertError(MIRAGE_MOBILE_INVALID_TEXTURE, label);
    }
    std::vector<uint8_t> out(width * height * 4, 0);
    const uint8_t* p = src;
    for (size_t by = 0; by < bh; ++by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            uint64_t alpha_word = ReadU64BE(p);
            uint64_t rgb_word = ReadU64BE(p + 8);
            p += 16;
            auto alphas = DecodeEacAlphaBlock(alpha_word);
            RgbBlock rgb = DecodeEtc2RgbBlock(rgb_word);
            RgbaBlock block{};
            for (int k = 0; k < 16; ++k) {
                block[k] = {rgb[k][0], rgb[k][1], rgb[k][2], alphas[k]};
            }
            WriteBlockColMajor(out.data(), width, height, bx, by, block);
        }
    }
    return out;
}

} // namespace mirage::mobile
