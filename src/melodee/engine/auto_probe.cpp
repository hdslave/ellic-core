// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/auto_probe.h"
#include <algorithm>
#include <cmath>

namespace melodee {

namespace {
constexpr float pi = 3.14159265358979f;
float semitones(float hz, float to) { return 12.0f * std::log2(hz / to); }
size_t span(const std::vector<float>& x, size_t from, size_t n) { return from >= x.size() ? 0 : std::min(n, x.size() - from); }
} // namespace

float probe_pitch(const std::vector<float>& x, size_t from, size_t n, float rate) {
    n = span(x, from, n);
    const size_t low = size_t(rate / 2000.0f), high = std::min(size_t(rate / 30.0f), n / 2);
    if (n < 64 || high <= low + 2) return 0;
    const float* s = x.data() + from;
    // The normalized square difference (McLeod): 1 where the sound repeats exactly after `lag`.
    std::vector<float> nsdf(high + 2, 0.0f);
    for (size_t lag = low; lag <= high + 1 && lag < n; ++lag) {
        double r = 0, m = 0;
        for (size_t i = 0; i + lag < n; ++i) { r += double(s[i]) * s[i + lag]; m += double(s[i]) * s[i] + double(s[i + lag]) * s[i + lag]; }
        nsdf[lag] = m > 1e-12 ? float(2 * r / m) : 0.0f;
    }
    float best = 0;
    for (size_t lag = low + 1; lag <= high; ++lag) best = std::max(best, nsdf[lag]);
    if (best < 0.8f) return 0; // nothing that repeats clearly enough: a noise, a drum, silence
    // The first peak near the best: the period, not a multiple of it.
    for (size_t lag = low + 1; lag <= high; ++lag) {
        if (nsdf[lag] < 0.9f * best || nsdf[lag] < nsdf[lag - 1] || nsdf[lag] < nsdf[lag + 1]) continue;
        const float a = nsdf[lag - 1], b = nsdf[lag], c = nsdf[lag + 1];
        const float d = a - 2 * b + c, shift = std::fabs(d) > 1e-9f ? 0.5f * (a - c) / d : 0.0f;
        return rate / (float(lag) + std::clamp(shift, -0.5f, 0.5f));
    }
    return 0;
}

float probe_db(const std::vector<float>& x, size_t from, size_t n) {
    n = span(x, from, n);
    double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += double(x[from + i]) * x[from + i];
    return n && sum > 1e-14 * n ? float(10.0 * std::log10(sum / double(n))) : -120.0f;
}

float probe_brightness(const std::vector<float>& x, size_t from, size_t n, float rate) {
    n = span(x, from, n);
    double level = 0, slope = 0;
    for (size_t i = 1; i < n; ++i) {
        const double d = double(x[from + i]) - x[from + i - 1];
        level += double(x[from + i]) * x[from + i]; slope += d * d;
    }
    return level > 1e-14 ? float(std::sqrt(slope / level) * rate / (2 * pi)) : 0.0f;
}

float probe_tone_db(const std::vector<float>& x, size_t from, size_t n, float hz, float rate) {
    n = span(x, from, n);
    if (!n || hz <= 0) return -120.0f;
    double power = 0;
    for (int h = 1; h <= 2; ++h) { // the fundamental and its octave (a fifth above has neither)
        const double w = 2 * pi * hz * h / rate, k = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (size_t i = 0; i < n; ++i) {
            const double window = 0.5 - 0.5 * std::cos(2 * pi * double(i) / double(n)); // Hann: a neighbour's leak kept out
            const double s0 = x[from + i] * window + k * s1 - s2;
            s2 = s1; s1 = s0;
        }
        power += s1 * s1 + s2 * s2 - k * s1 * s2;
    }
    return power > 1e-14 ? float(10.0 * std::log10(power / (double(n) * n))) : -120.0f;
}

ProbeGlide probe_glide(const std::vector<float>& x, const std::vector<float>& alone, size_t onset, float from_hz, float to_hz, float rate) {
    ProbeGlide g;
    if (from_hz <= 0 || to_hz <= 0 || onset >= x.size()) return g;
    const size_t window = size_t(0.03f * rate), hop = size_t(0.004f * rate);
    // Soon after the second key: mono when the first key's pitch is gone, where alone it still sounds (a poly
    // instrument plays it on under the second, however it decays).
    const size_t at = onset + size_t(0.15f * rate), n = size_t(0.06f * rate);
    const float kept = probe_tone_db(alone, at, n, from_hz, rate), heard = probe_tone_db(x, at, n, from_hz, rate);
    g.mono = kept > -90 && heard < kept - 10;
    if (!g.mono) return g;
    // The pitch from the second key on: the time it takes to be at the second key's, and stay there.
    size_t there = 0;
    unsigned steady = 0;
    for (size_t at = onset; at + window < x.size() && at < onset + size_t(2.0f * rate); at += hop) {
        const float hz = probe_pitch(x, at, window, rate);
        const bool near = hz > 0 && std::fabs(semitones(hz, to_hz)) < 0.4f;
        if (near) { if (!steady++) there = at; }
        else steady = 0;
        if (steady >= 4) break;
    }
    if (steady < 4) return g; // never settled: no time to give
    // The window's middle is where its pitch was; a jump reads there in the first window (its middle, 15 ms on).
    const float seconds = float(there - onset + window / 2) / rate;
    g.seconds = seconds > 0.025f ? seconds : 0.0f;
    return g;
}

bool probe_slides_apart(const std::vector<float>& x, size_t onset, float from_hz, float to_hz, float rate) {
    if (from_hz <= 0 || to_hz <= 0) return false;
    const float hz = probe_pitch(x, onset + size_t(0.005f * rate), size_t(0.025f * rate), rate);
    return hz > 0 && std::fabs(semitones(hz, to_hz)) > 1.0f && std::fabs(semitones(hz, from_hz)) < std::fabs(semitones(hz, to_hz));
}

uint8_t probe_accent(const std::vector<uint8_t>& velocities, const std::vector<float>& db, const std::vector<float>& bright) {
    const size_t n = std::min({velocities.size(), db.size(), bright.size()});
    if (n < 4) return 0;
    // How far each step up moves the sound: its level, and its brightness either way (an accent can open the filter
    // wider and so sound darker in its first moments), a semitone of brightness half a dB.
    std::vector<float> step;
    for (size_t i = 0; i + 1 < n; ++i) {
        const float louder = std::fabs(db[i + 1] - db[i]);
        const float brighter = bright[i] > 0 && bright[i + 1] > 0 ? std::fabs(semitones(bright[i + 1], bright[i])) : 0.0f;
        step.push_back(louder + 0.5f * brighter);
    }
    const size_t at = size_t(std::max_element(step.begin(), step.end()) - step.begin());
    std::vector<float> others;
    for (size_t i = 0; i < step.size(); ++i) if (i != at) others.push_back(step[i]);
    std::nth_element(others.begin(), others.begin() + others.size() / 2, others.end());
    const float usual = others[others.size() / 2];
    // A jump of its own: well over the usual step and over the steps either side (a velocity curve's first steps are
    // big too, but so are their neighbours).
    const float beside = std::max(at > 0 ? step[at - 1] : 0.0f, at + 1 < step.size() ? step[at + 1] : 0.0f);
    return step[at] >= 1.0f && step[at] >= 4.0f * std::max(usual, 0.25f) && step[at] >= 3.0f * std::max(beside, 0.25f) ? velocities[at + 1] : 0;
}

bool probe_drums(const std::vector<uint8_t>& keys, const std::vector<float>& hz) {
    const size_t n = std::min(keys.size(), hz.size());
    if (n < 3) return false;
    // Each pitched key against each other: in tune with the keys between them, an octave either way allowed (a pitch
    // read an octave off).
    unsigned best = 0;
    for (size_t r = 0; r < n; ++r) {
        if (hz[r] <= 0) continue;
        unsigned follow = 0;
        for (size_t k = 0; k < n; ++k) {
            if (hz[k] <= 0) continue;
            float off = semitones(hz[k], hz[r]) - float(int(keys[k]) - int(keys[r]));
            off -= 12.0f * std::round(off / 12.0f);
            follow += std::fabs(off) < 0.7f;
        }
        best = std::max(best, follow);
    }
    return best * 2 < n + 1; // fewer than half in tune with each other
}

bool probe_varies(const std::vector<std::vector<float>>& hits, float rate) {
    if (hits.size() < 2) return false;
    const size_t frame = size_t(0.01f * rate), frames = 30; // 300 ms, 10 ms at a time
    auto envelope = [&](const std::vector<float>& x) {
        std::vector<float> e;
        for (size_t f = 0; f < frames; ++f) e.push_back(probe_db(x, f * frame, frame));
        return e;
    };
    const auto first = envelope(hits[0]);
    const float bright = probe_brightness(hits[0], 0, size_t(0.1f * rate), rate);
    for (size_t h = 1; h < hits.size(); ++h) {
        const auto other = envelope(hits[h]);
        double apart = 0;
        unsigned counted = 0;
        for (size_t f = 0; f < frames; ++f) {
            if (first[f] < -40 || other[f] < -40) continue; // the quiet ends, where noise is all there is
            apart += std::fabs(first[f] - other[f]);
            ++counted;
        }
        if (counted && apart / counted > 1.0) return true;
        const float b = probe_brightness(hits[h], 0, size_t(0.1f * rate), rate);
        if (bright > 0 && b > 0 && std::fabs(b / bright - 1.0f) > 0.06f) return true;
    }
    return false;
}

} // namespace melodee
