// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/root_find.h"
#include "melodee/engine/dsp.h"
#include "melodee/engine/fft.h"
#include <algorithm>
#include <cmath>
#include <complex>

// A sample's note: its pitch in windows of 93 ms over its loud part, by the
// normalized square difference (McLeod and Wyvill, 2005, as probe_pitch() in
// engine/auto_probe.cpp, here through the FFT), read at half of 44.1 kHz. A
// window counts when its sound repeats clearly; the sample has a note when most
// of its loud windows count and most of those agree on one pitch class (an
// octave either way is the same note). A drum, a noise, a phrase of several
// notes, a sound whose pitch glides and most chords have none: they play as a
// C, never as a wrong note. On 172 multisampled synth notes (the auto sampler's,
// named by their keys) 150 came out right and 22 none, none wrong.
namespace melodee {
namespace {

using Complex = std::complex<float>;
constexpr unsigned factor = 2;                         // read at 22.05 kHz
constexpr float rate = dsp::model_rate / factor;
constexpr unsigned window = 2048;                      // 93 ms: two periods of the lowest note
constexpr unsigned size = 2 * window;                  // the FFT: padded, so the sound does not wrap onto itself
constexpr unsigned hop = window / 2;
constexpr unsigned most_windows = 48;                  // spread over the loud part when it has more
constexpr unsigned most_frames = unsigned(8 * dsp::model_rate); // the first 8 seconds
constexpr float lowest_hz = 26.0f, highest_hz = 4200.0f; // A0 (27.5 Hz) to C8 (4186 Hz), with room
constexpr float loud = 0.0316f;                        // -30 dB under the loudest window: the rest is its tail or silence
constexpr float clear = 0.8f;                          // how exactly a window repeats to count (1: exactly)
constexpr float counted = 0.5f, agreeing = 0.7f;       // the shares of loud windows that count and of those that agree

// The sound low-passed under 10 kHz and kept every second frame: a windowed sinc, 63 taps.
std::vector<float> decimate(const std::vector<float>& mono, size_t frames) {
    constexpr int taps = 63, half = taps / 2;
    static const std::vector<float> kernel = [] {
        std::vector<float> k(taps);
        const double cut = 10000.0 / dsp::model_rate;
        double sum = 0;
        for (int n = 0; n < taps; ++n) {
            const double x = n - half, sinc = x == 0 ? 2 * cut : std::sin(2 * M_PI * cut * x) / (M_PI * x);
            const double blackman = 0.42 - 0.5 * std::cos(2 * M_PI * n / (taps - 1)) + 0.08 * std::cos(4 * M_PI * n / (taps - 1));
            k[size_t(n)] = float(sinc * blackman);
            sum += sinc * blackman;
        }
        for (auto& v : k) v = float(v / sum);
        return k;
    }();
    std::vector<float> out(frames / factor);
    for (size_t m = 0; m < out.size(); ++m) {
        const long centre = long(m * factor);
        float acc = 0;
        for (int n = 0; n < taps; ++n) {
            const long at = centre + n - half;
            if (at >= 0 && size_t(at) < frames) acc += mono[size_t(at)] * kernel[size_t(n)];
        }
        out[m] = acc;
    }
    return out;
}

// One window's pitch in Hz and how clearly it repeats (0..1); 0 Hz when it does not.
struct Heard { float hz = 0, clarity = 0; };
Heard window_pitch(const float* x, std::vector<Complex>& buffer, const std::vector<Complex>& twiddle, std::vector<float>& nsdf) {
    for (unsigned n = 0; n < size; ++n) buffer[n] = n < window ? Complex(x[n], 0.0f) : Complex(0.0f, 0.0f);
    fft(buffer, twiddle);
    for (auto& z : buffer) z = Complex(std::norm(z), 0.0f); // its power; the inverse of it is the autocorrelation
    fft(buffer, twiddle);                                    // a real, even spectrum: forward is the inverse times `size`
    const unsigned high = std::min(unsigned(std::ceil(rate / lowest_hz)), window / 2), low = unsigned(rate / highest_hz);
    double m = 0;
    for (unsigned n = 0; n < window; ++n) m += 2.0 * double(x[n]) * x[n];
    nsdf.assign(high + 2, 0.0f);
    for (unsigned lag = 0; lag < high + 2; ++lag) {
        if (lag) m -= double(x[lag - 1]) * x[lag - 1] + double(x[window - lag]) * x[window - lag];
        nsdf[lag] = m > 1e-12 ? float(2.0 * buffer[lag].real() / size / m) : 0.0f;
    }
    // The peaks of each rise above zero after the first fall below it; the first within 0.9 of the highest is the
    // period (a later one is a multiple of it).
    unsigned lag = 1;
    while (lag <= high && nsdf[lag] > 0) ++lag;
    float best = 0;
    std::vector<unsigned> peaks;
    while (lag <= high) {
        while (lag <= high && nsdf[lag] <= 0) ++lag;
        unsigned top = 0;
        for (; lag <= high && nsdf[lag] > 0; ++lag) if (!top || nsdf[lag] > nsdf[top]) top = lag;
        if (top >= 2 && top < high) { peaks.push_back(top); best = std::max(best, nsdf[top]); }
    }
    if (best < clear) return {};
    for (unsigned top : peaks) {
        if (nsdf[top] < 0.9f * best) continue;
        if (top < low) return {}; // higher than the range: a later peak would be a multiple of it, a wrong note
        const float a = nsdf[top - 1], b = nsdf[top], c = nsdf[top + 1];
        const float d = a - 2 * b + c, shift = std::fabs(d) > 1e-9f ? std::clamp(0.5f * (a - c) / d, -0.5f, 0.5f) : 0.0f;
        const float hz = rate / (float(top) + shift);
        return hz >= lowest_hz && hz <= highest_hz ? Heard {hz, b} : Heard {};
    }
    return {};
}

// How far apart two pitch classes are (semitones, 0..6), an octave either way being the same.
float apart(float a, float b) {
    const float d = std::fmod(std::fabs(a - b), 12.0f);
    return std::min(d, 12.0f - d);
}

} // namespace

SampleRoot find_root(const std::vector<float>& model_mono) {
    const auto x = decimate(model_mono, std::min<size_t>(model_mono.size(), most_frames));
    if (x.size() < window) return {};
    const unsigned count = unsigned((x.size() - window) / hop + 1);
    std::vector<float> level(count);
    float loudest = 0;
    for (unsigned w = 0; w < count; ++w) {
        double sum = 0;
        for (unsigned n = 0; n < window; ++n) sum += double(x[w * hop + n]) * x[w * hop + n];
        level[w] = float(std::sqrt(sum / window));
        loudest = std::max(loudest, level[w]);
    }
    if (loudest < 1e-4f) return {}; // silence
    std::vector<unsigned> chosen;
    for (unsigned w = 0; w < count; ++w) if (level[w] >= loud * loudest) chosen.push_back(w);
    if (chosen.size() > most_windows) {
        std::vector<unsigned> spread(most_windows);
        for (unsigned k = 0; k < most_windows; ++k) spread[k] = chosen[size_t(k) * chosen.size() / most_windows];
        chosen.swap(spread);
    }
    std::vector<Complex> twiddle(size / 2), buffer(size);
    for (unsigned k = 0; k < size / 2; ++k) twiddle[k] = std::polar(1.0f, float(-2 * M_PI * k / size));
    std::vector<float> nsdf, classes; // each window that counts: its pitch as a MIDI note, within an octave (0..12)
    for (unsigned w : chosen) {
        const Heard heard = window_pitch(&x[size_t(w) * hop], buffer, twiddle, nsdf);
        if (heard.hz <= 0) continue;
        const float note = 69.0f + 12.0f * std::log2(heard.hz / 440.0f);
        classes.push_back(std::fmod(std::fmod(note, 12.0f) + 12.0f, 12.0f));
    }
    if (classes.size() < 3 || float(classes.size()) < counted * float(chosen.size())) return {}; // mostly no pitch
    // The pitch class most windows lie within half a semitone of, then their mean (around it, not across 0).
    size_t most = 0;
    float centre = 0;
    for (float c : classes) {
        size_t near = 0;
        for (float o : classes) near += apart(c, o) <= 0.5f;
        if (near > most) { most = near; centre = c; }
    }
    if (float(most) < agreeing * float(classes.size())) return {}; // several notes
    double sum = 0;
    for (float o : classes) {
        if (apart(centre, o) > 0.5f) continue;
        float d = o - centre;
        if (d > 6) d -= 12;
        if (d < -6) d += 12;
        sum += d;
    }
    const double mean = centre + sum / double(most);
    const double nearest = std::round(mean);
    SampleRoot out;
    out.pitch_class = int((long(nearest) % 12 + 12) % 12);
    out.cents = int(std::lround((mean - nearest) * 100.0));
    return out;
}

} // namespace melodee
