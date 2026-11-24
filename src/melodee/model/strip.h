// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <cstdint>

// The mixer's strip (melodee-sample docs/MIXER_PLAN.md): the same fixed processing on every channel of the mix
// (tracks, submixes, buses, the input and the master), after the channel's effect chain: trim, a nine-band EQ, a
// compressor and width. As it comes, a strip changes nothing, so a project from before it plays as it did.
namespace melodee {

constexpr unsigned eq_bands = 9;
// The bands' centres in Hz, an octave apart.
constexpr std::array<uint16_t, eq_bands> eq_band_hz {63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
constexpr int eq_range = 120;       // a band's gain, tenths of a dB: -12..12 dB
constexpr int trim_range = 240;     // tenths of a dB: -24..24 dB
constexpr unsigned width_max = 200; // percent: 0 mono, 100 as it is, 200 twice as wide

// The compressor's attack and release, as one choice.
enum class CompSpeed : uint8_t { Fast, Medium, Slow, Auto, Count };
constexpr unsigned comp_amount_max = 48;                  // dB below full scale
constexpr unsigned comp_ratio_min = 15, comp_ratio_max = 200; // tenths: 1.5:1 .. 20:1
constexpr int comp_gain_min = -120, comp_gain_max = 240;  // tenths of a dB

struct Strip {
    int16_t trim = 0;                    // tenths of a dB, before the bands
    uint8_t width = 100;                 // percent
    bool eq_on = true;                   // off: the bands are skipped, their gains kept
    std::array<int16_t, eq_bands> eq {}; // each band's gain, tenths of a dB
    bool comp_on = true;                 // off: the compressor is skipped, its values kept
    uint8_t comp_amount = 0;             // where it starts, dB below full scale; 0: it passes the sound as it is
    uint8_t comp_ratio = 40;             // tenths
    uint8_t comp_speed = uint8_t(CompSpeed::Medium);
    uint8_t comp_key = 0;                // what it hears: 0 its own channel, else fx_key_track(t) or fx_key_bus(b) (project.h)
    uint8_t comp_mix = 100;              // percent of the compressed sound against the dry
    int16_t comp_gain = 0;               // after it, tenths of a dB

    bool eq_flat() const { for (auto g : eq) if (g) return false; return true; }
    bool comp_idle() const { return comp_amount == 0; }
    // As it comes: the file leaves it out.
    bool flat() const {
        const Strip none;
        return trim == none.trim && width == none.width && eq_on == none.eq_on && eq == none.eq && comp_on == none.comp_on
            && comp_amount == none.comp_amount && comp_ratio == none.comp_ratio && comp_speed == none.comp_speed
            && comp_key == none.comp_key && comp_mix == none.comp_mix && comp_gain == none.comp_gain;
    }
};

} // namespace melodee
