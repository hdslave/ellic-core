// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// The auto sampler's check (shell/print_engine.cpp probe_track()): what a
// plug-in is, heard from a few notes, so AUTO SAMPLE needs no choice and reads
// nothing from the plug-in. Each finding is worked out here from the audio
// alone (mono, at the device's rate), so the tests can hand it made-up sounds.
namespace melodee {

// A fundamental by autocorrelation over x[from, from + n): 30 Hz to 2 kHz; 0 when the sound has none clear enough
// (a noise, a drum, silence).
float probe_pitch(const std::vector<float>& x, size_t from, size_t n, float rate);
// The level of x[from, from + n) in dB (RMS), -120 for silence.
float probe_db(const std::vector<float>& x, size_t from, size_t n);
// How bright: the RMS of the sound's slope against its RMS, as a frequency (Hz). A brighter sound reads higher.
float probe_brightness(const std::vector<float>& x, size_t from, size_t n, float rate);
// The level of one frequency and its octave in x[from, from + n), dB.
float probe_tone_db(const std::vector<float>& x, size_t from, size_t n, float hz, float rate);

// Two keys played legato (`x`: the second pressed at `onset` while the first, at `from_hz`, is held; it sounds `to_hz`
// on its own), and the first key held alone the same way (`alone`): mono when the first key's pitch is gone soon after
// the second, as it is not alone, and then the seconds its pitch took to slide to the second's (0: at once).
struct ProbeGlide {
    bool mono = false;
    float seconds = 0;
};
ProbeGlide probe_glide(const std::vector<float>& x, const std::vector<float>& alone, size_t onset, float from_hz, float to_hz, float rate);
// The second of two keys played apart (it starts at `onset`): whether it starts at the first key's pitch and slides
// (a portamento that is not legato).
bool probe_slides_apart(const std::vector<float>& x, size_t onset, float from_hz, float to_hz, float rate);

// One key at rising velocities (`velocities`, each with its level in dB and brightness): the velocity a step up starts
// the accent at, where the sound jumps far more than between any other two; 0 for none.
uint8_t probe_accent(const std::vector<uint8_t>& velocities, const std::vector<float>& db, const std::vector<float>& bright);

// Keys played one at a time, each with the pitch heard (0 none): drums when most do not follow the keys as a
// pitched instrument does (an octave up twice the frequency).
bool probe_drums(const std::vector<uint8_t>& keys, const std::vector<float>& hz);

// The same key hit a few times: whether the hits differ (a level envelope or brightness apart by more than a
// plug-in's own noise), so each key wants takes of its own. Phase alone does not count.
bool probe_varies(const std::vector<std::vector<float>>& hits, float rate);

} // namespace melodee
