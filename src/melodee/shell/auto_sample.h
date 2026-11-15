// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The auto sampler: a plug-in track's instrument played a key at a time by
// the print engine (shell/print_engine.h, a print host of its own, at SCHED_IDLE),
// each note held, let go and its release kept, written as 24-bit WAVs and an MPC
// keygroup program (shell/xpm_write.h) in Samples/Keygroups/<name>/, which a new
// KEYGROUP track then plays with the plug-in track's patterns and mix
// (shell/auto_sample.cpp). Taken before the track's chain (the keygroup
// track gets the chain) or after it (the keygroup track's chain empty).
namespace melodee {

// What the check heard (engine/auto_probe.h), before anything is sampled: nothing to pick, nothing read from the
// plug-in. Keys: a pitched instrument, as before. Mono: one note at a time; `glide` the seconds its pitch slides to a
// new key, `legato` only when the keys overlap (the program is written Mono with that Portamento). Drums: each key a
// sound of its own, sampled once each, not stretched over its neighbours; `keys` those of the range that sound.
// `accent`: the velocity the sound jumps at (the layers split there), 0 none. `takes`: hits of one key differ, so each
// key is taken this many times and the program plays them in turn. `sustains`: a held key keeps its level, so LOOP
// (at Auto) loops each note's held part and it plays on for as long as the key is held.
enum class AutoKind : uint8_t { Keys, Mono, Drums };
struct AutoFindings {
    bool heard = false;
    AutoKind kind = AutoKind::Keys;
    float glide = 0;
    bool legato = true;
    uint8_t accent = 0;
    uint8_t takes = 1;
    bool sustains = false;
    std::array<uint64_t, 2> keys {};
    uint8_t scan_low = 0, scan_high = 127; // drums: the keys the check tried (outside them nothing is known)
    bool sounds(unsigned key) const {
        if (key > 127) return false;
        return key < scan_low || key > scan_high || ((keys[key / 64] >> (key % 64)) & 1);
    }
    unsigned sounding() const { unsigned n = 0; for (auto k : keys) n += unsigned(__builtin_popcountll(k)); return n; }
};

// What a plug-in track is played over, a note at a time.
struct AutoSampleSettings {
    uint8_t low = 36, high = 96; // the lowest and highest root, MIDI notes (C2, C7)
    uint8_t step = 3;            // semitones between roots
    uint8_t layers = 1;          // velocity layers, 1-4
    float hold = 2.0f;           // seconds each key is held
    float tail = 8.0f;           // seconds of release kept at most
    bool post_fx = false;        // taken after the track's chain (its effects in the samples), else before it
    bool loop = false;           // each sample a loop where the key was held (engine/loop_find.h), when not at Auto
    bool loop_auto = true;       // LOOP at Auto: a loop when the check heard the sound sustain (AutoFindings::sustains)
    bool normalize = true;       // one gain for the whole program, its loudest sample at -1 dB (layers keep their balance)
    AutoFindings found;          // what the check heard
    bool looping() const { return loop_auto ? found.sustains && found.kind != AutoKind::Drums : loop; }
    // The velocity layers taken: as asked, two at least where the check heard an accent.
    unsigned layer_count() const { return std::clamp<unsigned>(found.accent && layers < 2 ? 2 : layers, 1, 4); }
    // Takes of each key: the check's, with one velocity layer (MPC plays a keygroup's layers in turn or by velocity, not both).
    unsigned take_count() const { return layer_count() == 1 ? std::clamp<unsigned>(found.takes, 1, 4) : 1; }
    // Each layer's velocity (the loudest of its range): 127 alone; 63, 127; 42, 84, 127; 31, 63, 95, 127. With an
    // accent the top layer is the accent (127) and the rest share the velocities under it.
    uint8_t velocity(unsigned layer) const {
        const unsigned n = layer_count();
        layer = std::min(layer, n - 1);
        if (found.accent > 1 && found.accent <= 127) {
            if (layer + 1 == n) return 127;
            return uint8_t(std::clamp<unsigned>((layer + 1) * (found.accent - 1u) / (n - 1), 1, 126));
        }
        return uint8_t(std::clamp<unsigned>((layer + 1) * 127 / n, 1, 127));
    }
    // The keys sampled: every STEP from LOW; drums, every key of the range that sounds.
    std::vector<uint8_t> roots() const {
        std::vector<uint8_t> out;
        if (found.kind == AutoKind::Drums) {
            for (int k = low; k <= high && k <= 127; ++k) if (found.sounds(unsigned(k))) out.push_back(uint8_t(k));
            return out;
        }
        for (int k = low; k <= high && k <= 127; k += std::max<int>(step, 1)) out.push_back(uint8_t(k));
        return out;
    }
};

// What the check heard of a plug-in track, or why it could not.
struct AutoProbeResult {
    std::string project_path;
    unsigned track = 0;
    AutoFindings found;
    std::string error;
    std::string heard; // what it measured, for `melodee-print status`
};

// What the auto sampler made: "<folder>/<name>.xpm" and its WAVs beside it, or why not.
struct AutoSampleResult {
    std::string project_path;
    unsigned track = 0;
    std::string program; // the .xpm's path
    std::string name;
    std::string error;   // empty: made
    unsigned samples = 0;
    unsigned loops = 0;  // samples given a loop
    unsigned centred = 0; // samples whose DC offset was taken out
    float gain_db = 0;   // the program's gain (normalize)
    bool post_fx = false;
    double seconds = 0;
};

} // namespace melodee
