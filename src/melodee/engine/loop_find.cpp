// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/loop_find.h"
#include <algorithm>
#include <cmath>

namespace melodee {

namespace {
constexpr float silence = 0.000316f; // -70 dB
constexpr float good_enough = 0.6f;  // the weakest match kept

// The rising zero crossings of `m` in [from, to).
std::vector<uint32_t> rising(const std::vector<float>& m, uint64_t from, uint64_t to) {
    std::vector<uint32_t> out;
    for (uint64_t f = std::max<uint64_t>(from, 1); f < to && f < m.size(); ++f)
        if (m[f - 1] < 0 && m[f] >= 0) out.push_back(uint32_t(f));
    return out;
}

// How alike the `w` frames before a and before b are: their correlation, normalised, 0..1.
float alike(const std::vector<float>& m, uint32_t a, uint32_t b, unsigned w) {
    double ab = 0, aa = 0, bb = 0;
    for (unsigned i = 1; i <= w; ++i) {
        double x = m[a - i], y = m[b - i];
        ab += x * y; aa += x * x; bb += y * y;
    }
    if (aa <= 0 || bb <= 0) return 0;
    return float(std::max(0.0, ab / std::sqrt(aa * bb)));
}
} // namespace

LoopPoints find_loop(std::vector<float>& left, std::vector<float>& right, uint64_t held, float rate) {
    LoopPoints out;
    const size_t frames = std::min(left.size(), right.size());
    held = std::min<uint64_t>(held, frames);
    if (rate <= 0 || held < uint64_t(0.3f * rate)) return out;
    std::vector<float> m(static_cast<size_t>(held));
    float peak = 0;
    for (size_t f = 0; f < m.size(); ++f) { m[f] = 0.5f * (left[f] + right[f]); peak = std::max(peak, std::fabs(m[f])); }
    if (peak < silence) return out;
    const uint64_t shortest = uint64_t(0.1f * rate), longest = std::min<uint64_t>(uint64_t(1.0f * rate), held * 3 / 4);
    const unsigned window = unsigned(std::min<uint64_t>(uint64_t(0.02f * rate), shortest / 2)); // 20 ms compared
    // The end: the last rising crossing at least 10 ms before the key was let go (its release starts after).
    const uint64_t latest = held - uint64_t(0.01f * rate);
    auto ends = rising(m, latest > uint64_t(0.05f * rate) ? latest - uint64_t(0.05f * rate) : 0, latest);
    if (ends.empty()) return out;
    const uint32_t end = ends.back();
    if (end < shortest + window) return out;
    const uint64_t earliest = std::max<uint64_t>({end > longest ? end - longest : 0, held / 4, uint64_t(window) * 2});
    auto starts = rising(m, earliest, end - shortest);
    // Many crossings (noise): every one compared would be slow; an even spread of 400 is enough.
    const size_t stride = std::max<size_t>(1, starts.size() / 400);
    float best = -1;
    uint32_t start = 0;
    for (size_t i = 0; i < starts.size(); i += stride) {
        float a = alike(m, starts[i], end, window);
        if (a > best) { best = a; start = starts[i]; }
    }
    if (best < good_enough || !start) return out;
    // The crossfade: the frames before the end turn into those before the start.
    const unsigned fade = unsigned(std::min<uint64_t>({uint64_t((end - start) / 4), uint64_t(0.05f * rate), start}));
    for (unsigned i = 0; i < fade; ++i) {
        const float t = float(i + 1) / float(fade + 1);
        const float keep = 1 - t, take = t; // the two places are alike: a straight fade keeps the level
        const size_t at = end - fade + i, from = start - fade + i;
        left[at] = keep * left[at] + take * left[from];
        right[at] = keep * right[at] + take * right[from];
    }
    out.start = start;
    out.end = end;
    out.match = best;
    return out;
}

} // namespace melodee
