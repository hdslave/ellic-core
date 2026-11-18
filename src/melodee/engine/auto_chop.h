// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/onsets.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/chop_slices.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Chop's Auto type: what kind of sound a sample is, read with its hits and beats when it loads (engine/onsets.cpp),
// and where Auto cuts it. DRUMS: a steady beat led by drums, a cut on each drum hit. MUSIC: a steady beat led by
// notes or chords, a cut where they change, then on the strongest hits. VOCAL: a voice, a cut where a phrase starts
// after a gap, then on words (cut as HITS for now). SHOTS: single sounds with silence between, one slice a sound. HITS: none of these, the strongest
// hits spread out.
namespace melodee {

enum class SoundKind : uint8_t { None, Drums, Music, Vocal, Shots, Hits, Count };
const char* sound_kind_name(SoundKind kind); // "DRUMS" ... "HITS"; "" for None

// The spectrum onsets.cpp finds hits in: `count` frames 10 ms apart, each `width` semitone bands (centred at `hz`),
// each band's level in 85 steps a tenfold (20 dB), 0 at 60 dB under the loudest band.
struct BandFrames {
    unsigned width = 0, count = 0;
    std::vector<uint8_t> level; // level[frame * width + band]
    std::vector<float> hz;
};

// A hit's sound just after its attack: its level in the low (under 150 Hz), mid (to 2.5 kHz) and high bands, in the
// same steps as BandFrames, and how long it rings: 10 ms steps until it falls 20 dB, or the next hit.
struct HitSound { uint8_t low = 0, mid = 0, high = 0, ring = 0; };

// From where to where a sound runs (44.1 kHz frames).
struct Span { uint32_t from = 0, to = 0; };

struct AutoRead {
    SoundKind kind = SoundKind::None;  // what Auto cuts it as
    SoundKind found = SoundKind::None; // what it reads as (VOCAL is cut as HITS until it scores well)
    float percussive = 0;            // the share of its sound in short hits rather than held notes (0..1)
    float voice = 0;                 // the share of its held sound between 150 Hz and 4 kHz (0..1)
    float slide = 0;                 // how often its strongest held pitch (150 Hz to 1.5 kHz) moves by a semitone from
                                     // one frame to the next, of its loud frames (a voice glides and wavers; keys hold or jump)
    std::vector<HitSound> sounds;    // one a hit, in the order find_hits_in() keeps them
    std::vector<Span> spans;         // its sounds between silences (50 dB under its loudest, 30 ms or more)
    std::vector<Span> phrases;       // its sound between gaps (35 dB under its loudest, 120 ms or more)
    std::vector<float> change;       // at each hit: how far the notes change there (0..1)
    int8_t bar_one = -1;             // which of the first four of BeatGrid::at is a bar's first beat; -1 not sure
};

// What find_hits_in() reads for Auto, from the sample's sides summed at 44.1 kHz, its spectrum, hits and beats.
void read_auto(const std::vector<float>& model_mono, const BandFrames& spectrum, const std::array<Hit, max_hits>& hits,
               unsigned hit_count, const BeatGrid& beats, AutoRead& out);

// SLICES (Shift + K3 on Auto): 8, 16, 32 or 64.
constexpr unsigned auto_counts = 4, auto_default_count = 1;
inline unsigned auto_slice_count(int amount) { return 8u << std::min(unsigned(std::max(amount, 0)), auto_counts - 1); }

// Auto's slices of [from, to) of a sample of `frames`: `want` of them (SHOTS: one a sound, whatever `want`). DRUMS
// and MUSIC keep to the whole bars of the region where the beat says which beat is a bar's first. `capped`: set when
// a sound found no room (64 slices).
Slices auto_slices(const AutoRead& read, unsigned want, uint32_t from, uint32_t to, uint32_t frames,
                   const std::array<Hit, max_hits>& hits, unsigned hit_count, const BeatGrid& beats, bool* capped = nullptr);

} // namespace melodee
