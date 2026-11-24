// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/channel_strip.h"
#include "melodee/engine/dsp.h"
#include "melodee/engine/rbj.h"
#include <algorithm>
#include <cmath>

namespace melodee {

namespace {
float db(float v) { return std::pow(10.0f, v / 20.0f); }
float follow(float seconds, float rate) { return std::exp(-1.0f / std::max(seconds * rate, 1.0f)); }
// `now` toward `target` by at most `step`.
float toward(float now, float target, float step) { return now < target ? std::min(target, now + step) : std::max(target, now - step); }
// Attack and release, seconds, for each speed; Auto's release goes from the first to the second as the reduction deepens.
constexpr float speed_attack[] {0.001f, 0.010f, 0.030f, 0.005f};
constexpr float speed_release[] {0.060f, 0.150f, 0.400f, 0.100f};
constexpr float auto_release_slow = 0.600f;
constexpr float knee_db = 6;

// One band's coefficients and both sides' state, for a block.
struct Live { float b0, b1, b2, a1, a2, zl1, zl2, zr1, zr2; };
// `N` bands, a frame at a time through all of them, both sides side by side. N fixed, so the compiler keeps every
// state in a register and unrolls the bands: one band's next frame never waits on the one before it, and the two
// sides fill each other's waits.
template <unsigned N> void run_bands(Live* bands, float* l, float* r, unsigned frames) {
    Live s[N];
    for (unsigned k = 0; k < N; ++k) s[k] = bands[k];
    for (unsigned f = 0; f < frames; ++f) {
        float xl = l[f], xr = r[f];
#pragma GCC unroll 16
        for (unsigned k = 0; k < N; ++k) {
            const float yl = s[k].b0 * xl + s[k].zl1, yr = s[k].b0 * xr + s[k].zr1;
            s[k].zl1 = s[k].b1 * xl - s[k].a1 * yl + s[k].zl2; s[k].zr1 = s[k].b1 * xr - s[k].a1 * yr + s[k].zr2;
            s[k].zl2 = s[k].b2 * xl - s[k].a2 * yl;            s[k].zr2 = s[k].b2 * xr - s[k].a2 * yr;
            xl = yl; xr = yr;
        }
        l[f] = xl; r[f] = xr;
    }
    for (unsigned k = 0; k < N; ++k) bands[k] = s[k];
}
using BandRunner = void (*)(Live*, float*, float*, unsigned);
constexpr BandRunner band_runners[eq_bands + 1] {nullptr, run_bands<1>, run_bands<2>, run_bands<3>, run_bands<4>,
                                                  run_bands<5>, run_bands<6>, run_bands<7>, run_bands<8>, run_bands<9>};
} // namespace

void ChannelStrip::set(const Strip& strip) {
    strip_ = strip;
    trim_target_ = strip.trim / 10.0f;
    for (unsigned n = 0; n < eq_bands; ++n) bands_[n].target = strip.eq_on ? strip.eq[n] / 10.0f : 0.0f;
    const bool on = strip.comp_on;
    blend_target_ = on && strip.comp_amount ? strip.comp_mix / 100.0f : 0.0f;
    makeup_target_ = on ? strip.comp_gain / 10.0f : 0.0f;
    threshold_db_ = -float(strip.comp_amount);
    slope_ = 1 - 10.0f / float(std::max<unsigned>(strip.comp_ratio, comp_ratio_min));
    width_target_ = strip.width / 100.0f;
    rate_ = 0; // speeds and the bands' coefficients worked out again at the next block
    idle_ = false;
}

void ChannelStrip::reset() {
    if (rate_ != dsp::sample_rate()) speeds();
    trim_db_ = trim_target_; width_ = width_target_; blend_ = blend_target_; makeup_db_ = makeup_target_;
    for (unsigned n = 0; n < eq_bands; ++n) {
        auto& b = bands_[n];
        b.gain = b.target;
        b.z[0][0] = b.z[0][1] = b.z[1][0] = b.z[1][1] = 0;
        b.running = b.gain != 0; b.quiet = 0;
        work_out(b, n);
    }
    env_ = 0; gain_ = 1; reduction_ = 0;
    check_idle();
}

void ChannelStrip::speeds() {
    rate_ = dsp::sample_rate();
    const unsigned s = std::min<unsigned>(strip_.comp_speed, unsigned(CompSpeed::Count) - 1);
    attack_ = follow(speed_attack[s], rate_);
    release_ = follow(speed_release[s], rate_);
    for (unsigned n = 0; n < eq_bands; ++n) work_out(bands_[n], n);
}

void ChannelStrip::work_out(Band& band, unsigned n) {
    float c[5];
    rbj::peak(c, float(eq_band_hz[n]) * std::min(1.0f, 0.45f * dsp::sample_rate() / float(eq_band_hz[n])), band.gain, band_q);
    band.b0 = c[0]; band.b1 = c[1]; band.b2 = c[2]; band.a1 = c[3]; band.a2 = c[4];
}

void ChannelStrip::check_idle() {
    bool bands = false;
    for (const auto& b : bands_) bands |= b.running;
    idle_ = !bands && trim_db_ == 0 && trim_target_ == 0 && width_ == 1 && width_target_ == 1 && blend_ == 0 && blend_target_ == 0
         && makeup_db_ == 0 && makeup_target_ == 0;
    if (idle_) { env_ = 0; gain_ = 1; reduction_ = 0; }
}

void ChannelStrip::process(float* l, float* r, unsigned frames, const float* key_l, const float* key_r) {
    if (idle_ || frames == 0) return;
    if (rate_ != dsp::sample_rate()) speeds();
    const float fr = float(frames);
    const float step_db = glide_db * fr / rate_;
    const float swing = fr / (glide_swing * rate_);

    // Trim.
    if (trim_db_ != 0 || trim_target_ != 0) {
        const float from = db(trim_db_);
        trim_db_ = toward(trim_db_, trim_target_, step_db);
        const float to = db(trim_db_), d = (to - from) / fr;
        for (unsigned f = 0; f < frames; ++f) { const float g = from + d * float(f + 1); l[f] *= g; r[f] *= g; }
    }

    // The bands in the chain (run_bands).
    Live live[eq_bands];
    unsigned at[eq_bands], count = 0;
    for (unsigned n = 0; n < eq_bands; ++n) {
        auto& b = bands_[n];
        if (b.gain != b.target) {
            b.gain = toward(b.gain, b.target, step_db);
            b.running = true;
            work_out(b, n);
        }
        if (!b.running) continue;
        live[count] = {b.b0, b.b1, b.b2, b.a1, b.a2, b.z[0][0], b.z[0][1], b.z[1][0], b.z[1][1]};
        at[count++] = n;
    }
    if (count) {
        band_runners[count](live, l, r, frames);
        for (unsigned k = 0; k < count; ++k) {
            auto& b = bands_[at[k]];
            const Live& s = live[k];
            b.z[0][0] = s.zl1; b.z[0][1] = s.zl2; b.z[1][0] = s.zr1; b.z[1][1] = s.zr2;
            // At 0 dB a band's output is its input but for rounding (about -90 dB): once its ringing has had time
            // to go, it leaves the chain.
            b.quiet = b.gain == 0 && b.target == 0 ? b.quiet + frames : 0;
            if (float(b.quiet) >= band_rest * rate_) {
                b.running = false; b.quiet = 0;
                b.z[0][0] = b.z[0][1] = b.z[1][0] = b.z[1][1] = 0;
            }
        }
    }

    // The compressor: the level heard followed a frame at a time, its gain worked out every comp_step frames and
    // eased between, blended against the dry, then the gain after it.
    reduction_ = 0;
    if (blend_ != 0 || blend_target_ != 0 || makeup_db_ != 0 || makeup_target_ != 0) {
        const float* hear_l = key_l ? key_l : l;
        const float* hear_r = key_r ? key_r : r;
        const float blend_from = blend_, makeup_from = db(makeup_db_);
        blend_ = toward(blend_, blend_target_, swing);
        makeup_db_ = toward(makeup_db_, makeup_target_, step_db);
        const float blend_d = (blend_ - blend_from) / fr, makeup_d = (db(makeup_db_) - makeup_from) / fr;
        const bool working = blend_from != 0 || blend_ != 0;
        for (unsigned f0 = 0; f0 < frames; f0 += comp_step) {
            const unsigned f1 = std::min(frames, f0 + comp_step);
            float red = 0;
            if (working) {
                for (unsigned f = f0; f < f1; ++f) {
                    const float level = std::max(std::fabs(hear_l[f]), std::fabs(hear_r[f]));
                    const float k = level > env_ ? attack_ : release_;
                    env_ = level + (env_ - level) * k;
                }
                if (env_ < 1e-12f) env_ = 0;
                const float over = 20 * std::log10(std::max(env_, 1e-6f)) - threshold_db_;
                red = over <= -knee_db / 2 ? 0 : over >= knee_db / 2 ? over * slope_
                    : slope_ * (over + knee_db / 2) * (over + knee_db / 2) / (2 * knee_db);
                if (strip_.comp_speed == uint8_t(CompSpeed::Auto)) { // a deeper reduction lets go more slowly
                    const float t = std::min(red / 12.0f, 1.0f);
                    release_ = follow(speed_release[unsigned(CompSpeed::Auto)] + t * (auto_release_slow - speed_release[unsigned(CompSpeed::Auto)]), rate_);
                }
            }
            const float aim = red != 0 ? db(-red) : 1.0f, from = gain_, d = (aim - from) / float(f1 - f0);
            for (unsigned f = f0; f < f1; ++f) {
                const float g = from + d * float(f - f0 + 1);
                const float bl = blend_from + blend_d * float(f + 1);
                const float m = makeup_from + makeup_d * float(f + 1);
                const float out = ((1 - bl) + bl * g) * m;
                l[f] *= out; r[f] *= out;
            }
            gain_ = aim;
            const float heard = (1 - blend_) + blend_ * aim;
            if (heard < 1) reduction_ = std::max(reduction_, -20 * std::log10(std::max(heard, 1e-6f)));
        }
        if (!working) { env_ = 0; gain_ = 1; }
    }

    // Width: the side's share, around the mid.
    if (width_ != 1 || width_target_ != 1) {
        const float from = width_;
        width_ = toward(width_, width_target_, swing * 2); // a full swing is 0..2
        const float d = (width_ - from) / fr;
        for (unsigned f = 0; f < frames; ++f) {
            const float w = from + d * float(f + 1);
            const float mid = (l[f] + r[f]) * 0.5f, side = (l[f] - r[f]) * 0.5f * w;
            l[f] = mid + side; r[f] = mid - side;
        }
    }
    check_idle();
}

} // namespace melodee
