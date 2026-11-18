// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/auto_chop.h"
#include "melodee/engine/onsets.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/chop_slices.h"
#include "melodee/model/sampler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Chop: where the slices of a pad's region start and end, as the MPC's Chop
// Mode cuts it (parity phase 5). THRESHOLD keeps the hits at least as strong
// as the threshold asks; REGIONS cuts it into equal parts; BEATS puts a
// slice on every division (1/16 ... 4 bars, as Simpler's) at the sample's own tempo, counted
// from its beat nearest the region's start (a lead-in before that beat a
// slice of its own), each cut on its tracked beat or a hit near it, or at the
// project's tempo from the region's start where the sample has none
// (Melodee's); MANUAL is what the
// pads tapped while it played, or what was edited by hand. Slices touch: one
// ends where the next starts, the last at `last`. The result lands on pads as
// regions of the same file, or one slice as a file of its own (Extract).
namespace melodee {

// Beats' divisions, as Simpler's Division: 1/16, 1/8, 1/4 (a beat), 1/2, 1 bar, 2 bars, 4 bars. Shift + K3 picks
// one; the slice is that many 16ths long, a bar as many as the project's time signature holds.
constexpr unsigned beat_divisions = 7, quarter_division = 2;
inline unsigned division_sixteenths(unsigned division, unsigned bar_sixteenths) {
    static constexpr unsigned notes[] {1, 2, 4, 8};
    division = std::min(division, beat_divisions - 1);
    return division < 4 ? notes[division] : std::max(1u, bar_sixteenths) << (division - 4);
}
// Its name on the card: the value, and "bar" or "bars" after it from 1 bar on.
inline const char* division_value(unsigned division) {
    static const char* names[] {"1/16", "1/8", "1/4", "1/2", "1", "2", "4"};
    return names[std::min(division, beat_divisions - 1)];
}
inline const char* division_unit(unsigned division) { return division < 4 ? "" : division == 4 ? "bar" : "bars"; }

// The cut nearest a tapped frame: the strongest hit from 50 ms before it to
// 10 ms after, else the tap itself (a tap comes a little after the sound).
inline uint32_t snap_to_hit(uint32_t frame, const std::array<Hit, max_hits>& hits, unsigned hit_count) {
    uint32_t from = frame > 2205 ? frame - 2205 : 0, to = frame + 441;
    int best = -1;
    for (unsigned n = 0; n < hit_count; ++n)
        if (hits[n].frame >= from && hits[n].frame <= to && (best < 0 || hits[n].strength > hits[size_t(best)].strength)) best = int(n);
    return best < 0 ? frame : hits[size_t(best)].frame;
}

// A Threshold amount (0..100) as a hit's strength (out of 255): the hits that chop would cut at.
inline uint8_t hit_floor(int threshold_amount) { return uint8_t(std::clamp(threshold_amount, 0, 100) * 255 / 100); }
// The hits Chop marks and SHIFT + the wheel jumps to: as strong as Threshold's amount, or 25% where that is higher (a
// strong amount leaves few hits on a busy sample; the ones heard should still count).
inline uint8_t mark_floor(int threshold_amount) { return hit_floor(std::min(threshold_amount, 25)); }
// The hit after (`way` > 0) or before `frame` at least 10 ms from it, as strong as `floor`; `frame` itself where there
// is none. 10 ms: a cut pulled onto a zero crossing just before a hit still counts as on it, so the next turn moves on.
inline uint32_t next_hit(uint32_t frame, int way, const std::array<Hit, max_hits>& hits, unsigned count, uint8_t floor) {
    constexpr uint32_t clear = 441;
    uint32_t best = frame;
    bool found = false;
    for (unsigned n = 0; n < count && n < max_hits; ++n) {
        const uint32_t h = hits[n].frame;
        if (!h || hits[n].strength < floor) continue;
        if (way > 0 ? h > frame + clear : h + clear < frame)
            if (!found || (way > 0 ? h < best : h > best)) { best = h; found = true; }
    }
    return best;
}

// The same inside a cut's room (`lo`..`hi`, short of the neighbouring cuts): `frame` itself where the next hit is not.
inline uint32_t next_hit_within(uint32_t frame, int way, const std::array<Hit, max_hits>& hits, unsigned count, uint8_t floor,
                                uint32_t lo, uint32_t hi) {
    const uint32_t h = next_hit(frame, way, hits, count, floor);
    return h != frame && h > lo && h < hi ? h : frame;
}

// Zero Snap (always on; Chop's B2 switch is gone): a cut pulled onto
// the nearest place the wave crosses zero, so a slice starts and stops
// without a click. The two sides are summed (left and right cross at
// different places; the sum is what a mono listener hears). It looks up to
// 5 ms before the cut and 1 ms after, and a crossing after counts double the
// distance: a cut sits on or just before a hit, and moving it into the hit
// would blunt it. Where nothing crosses (a DC offset, a deep bass held over
// the window) it takes the frame nearest zero. The slice edges' 2 ms fades
// (engine/sampler.cpp, Extract) catch what is left.
constexpr uint32_t zero_back = 220, zero_ahead = 44; // 5 ms and 1 ms at 44.1 kHz
inline uint32_t zero_crossing(const float* left, const float* right, uint32_t frames, uint32_t frame) {
    if (!frames || frame >= frames) return frame;
    auto sum = [&](uint32_t i) { return left[i] + right[i]; };
    uint32_t lo = frame > zero_back ? frame - zero_back : 0, hi = std::min(frames - 1, frame + zero_ahead);
    auto cost = [&](uint32_t i) { return i < frame ? frame - i : 2 * (i - frame); };
    uint32_t best = frame, quiet = frame;
    bool found = false;
    for (uint32_t i = lo; i <= hi; ++i) {
        float m = sum(i);
        if (std::fabs(m) < std::fabs(sum(quiet)) || (std::fabs(m) == std::fabs(sum(quiet)) && cost(i) < cost(quiet))) quiet = i;
        // A crossing between i - 1 and i (or a zero on i): the side of it nearer zero.
        if (i == 0) continue;
        float before = sum(i - 1);
        if (!(m == 0 || (before < 0) != (m < 0))) continue;
        uint32_t at = m == 0 || std::fabs(m) <= std::fabs(before) || i - 1 < lo ? i : i - 1;
        if (!found || cost(at) < cost(best)) { best = at; found = true; }
    }
    return found ? best : quiet;
}
// The same on a sample, `frame` and what comes back as the project counts them (44.1 kHz), read in its own frames.
inline uint32_t zero_crossing(const SampleData& sample, uint32_t frame) {
    const uint32_t own = sample.own(frame);
    const uint32_t at = zero_crossing(sample.left.data(), sample.right.data(), std::min<uint32_t>(sample.frames, uint32_t(sample.left.size())), own);
    return at == own ? frame : sample.model(at);
}

// The wheel's Zero Snap: the first crossing at or past `frame` going `way` (+1 later, -1 earlier), within 50 ms;
// `frame` itself where none is. The frame of the pair nearer zero, as above, but never back past `frame`.
constexpr uint32_t zero_reach = 2205;
inline uint32_t zero_crossing_toward(const float* left, const float* right, uint32_t frames, uint32_t frame, int way) {
    if (!frames || frame >= frames) return frame;
    auto sum = [&](uint32_t i) { return left[i] + right[i]; };
    for (uint32_t d = 0; d <= zero_reach; ++d) {
        int64_t i = way < 0 ? int64_t(frame) - d : int64_t(frame) + d;
        if (i < 1 || i >= int64_t(frames)) break;
        float m = sum(uint32_t(i)), before = sum(uint32_t(i - 1));
        if (!(m == 0 || (before < 0) != (m < 0))) continue;
        // The crossing lies between i - 1 and i: the side nearer zero, unless that steps back past `frame`.
        uint32_t at = m == 0 || std::fabs(m) <= std::fabs(before) ? uint32_t(i) : uint32_t(i - 1);
        if (way > 0 && at < frame) at = uint32_t(i);
        if (way < 0 && at > frame) at = uint32_t(i - 1);
        return at;
    }
    return frame;
}
inline uint32_t zero_crossing_toward(const SampleData& sample, uint32_t frame, int way) {
    const uint32_t own = sample.own(frame);
    const uint32_t at = zero_crossing_toward(sample.left.data(), sample.right.data(), std::min<uint32_t>(sample.frames, uint32_t(sample.left.size())),
                                             own, way);
    if (at == own) return frame;
    const uint32_t model = sample.model(at);
    return way > 0 ? std::max(model, frame) : std::min(model, frame); // never back past `frame`, as above
}

// The cut for a beat expected at `want`, a `step` after the cut before (`beat` frames a beat): the sample's tracked
// beat within an eighth of a beat of it and within 6% of the step, or within an eighth of a beat of `on_grid`, the
// same beat counted on the region's grid (so the slices follow a take that drifts, and a beat a drummer played
// late after one played early, yet a tracker that found the off-beats, a 16th or more away, or squeezed its last
// beats to reach a hit, never pulls them off the region's own beat), else the hit within 30 ms (a quarter of the step
// where that is less), else `want` itself. `tracked`: it was a beat.
inline uint32_t beat_cut(double want, double step, double beat, const BeatGrid& grid, const std::array<Hit, max_hits>& hits,
                         unsigned hit_count, bool& tracked, double on_grid = -1) {
    auto gap = [&](uint32_t frame) { return std::fabs(double(frame) - want); };
    tracked = false;
    auto b = std::lower_bound(grid.at.begin(), grid.at.end(), uint32_t(want));
    uint32_t best = uint32_t(want);
    double nearest = 1e300;
    for (auto it : {b, b == grid.at.begin() ? b : b - 1, b + 1 < grid.at.end() ? b + 1 : b})
        if (it != grid.at.end() && gap(*it) < nearest
            && (gap(*it) <= std::min(beat / 8, step * 0.06) || (on_grid >= 0 && std::fabs(double(*it) - on_grid) <= beat / 8))) {
            nearest = gap(*it); best = *it; tracked = true;
        }
    if (tracked) return best;
    nearest = std::min(1323.0, step / 4); // 30 ms, or a quarter of the step where that is less (a fast 1/16)
    for (unsigned n = 0; n < hit_count; ++n)
        if (hits[n].frame && gap(hits[n].frame) <= nearest) { nearest = gap(hits[n].frame); best = hits[n].frame; }
    return best;
}

// The slices of [from, to) of a sample of `frames`. `amount`: THRESHOLD
// 0..100 (as on the MPC, the higher, the fewer slices: 0 keeps every hit), REGIONS the number of slices 2..64, BEATS
// 16ths a slice 1..256 (division_sixteenths(); `grid`: the sample's own, else `tempo_tenths`, the project's). MANUAL starts as one slice, the whole region.
// `capped`: set when a cut was left out because the slices were full (64), so the last slice runs on past where it should end.
inline Slices chop(ChopMode mode, int amount, uint32_t from, uint32_t to, uint32_t frames, const std::array<Hit, max_hits>& hits,
                   unsigned hit_count, unsigned tempo_tenths, const BeatGrid* grid = nullptr, bool* capped = nullptr) {
    Slices s;
    if (capped) *capped = false;
    if (!frames) return s;
    to = to && to <= frames ? to : frames;
    from = std::min(from, to > min_slice ? to - min_slice : 0);
    s.whole(from, to);
    auto cut = [&](uint32_t frame) { // a cut, or a note that there was no room for it
        if (s.count >= max_slices) { if (capped) *capped = true; return; }
        s.add(frame, frames);
    };
    switch (mode) {
    case ChopMode::Threshold: {
        int threshold = std::clamp(amount, 0, 100) * 255 / 100;
        for (unsigned n = 0; n < hit_count; ++n)
            if (hits[n].frame > from && hits[n].strength >= threshold) cut(hits[n].frame);
        break;
    }
    case ChopMode::Regions: {
        unsigned parts = unsigned(std::clamp(amount, 2, int(max_slices)));
        for (unsigned n = 1; n < parts; ++n) s.add(from + uint32_t(uint64_t(to - from) * n / parts), frames);
        break;
    }
    case ChopMode::Beats: {
        if (grid && grid->found()) { // a slice every `amount` of the sample's beats, from the beat nearest the region's start
            const double beat = 44100.0 * 600 / grid->tempo, step = beat * std::clamp(amount, 1, 256) / 4;
            const double reach = std::min(beat / 8, step * 0.06); // a beat this near the end is the end
            // Counted from the beat nearest the region's start, or where the steady beat starts if that is later
            // (a free intro before it is one slice); the last cut where its last beat ends (a free outro after it
            // is one slice).
            double start = double(grid->pulse_from) > from + beat / 2 ? double(grid->pulse_from) : double(from);
            double at = grid->first + std::round((start - grid->first) / beat) * beat;
            const double last = std::min(double(to), double(grid->pulse_to) + beat * 1.5);
            bool on_beat = false;
            uint32_t anchor = beat_cut(at, step, beat, *grid, hits, hit_count, on_beat);
            if (on_beat) at = anchor;
            if (at > from + reach && at + reach < to) cut(anchor); // a lead-in before that beat: a slice of its own
            double counted = at; // the same beats on the region's grid
            for (at += step; at + reach < last;) {
                bool tracked = false;
                counted += step;
                uint32_t frame = beat_cut(at, step, beat, *grid, hits, hit_count, tracked, counted);
                if (frame >= last) break;
                cut(frame);
                at = (tracked ? frame : at) + step; // the next from a tracked beat; from the grid past a hit
            }
            break;
        }
        double beat = 44100.0 * 60.0 / (std::max(tempo_tenths, 10u) / 10.0) * std::clamp(amount, 1, 256) / 4; // a slice's length
        for (double at = from + beat; at < to; at += beat) cut(uint32_t(at));
        break;
    }
    default: break;
    }
    return s;
}
// A new chop with the cuts made by hand (Simpler's manual slices kept whatever the sensitivity): those first, then
// each of `cut`'s own not taken out by hand and not near a hand cut: closer to one than a quarter of the type's own
// spacing there (the shorter way to its neighbours in `cut`), it is that cut, placed by hand, and gives way, so no
// sliver is left between a beat line and a hand cut just after it. The region stays `cut`'s. `capped` as chop()'s:
// set when a cut found no room.
// How near a hand cut the type's cut n (of `cut`) gives way to it: a quarter of the type's own spacing there, 20 ms at least.
inline uint32_t give_way_reach(const Slices& cut, unsigned n, uint32_t frames) {
    const uint32_t at = cut.start[n], next = n + 1 < cut.count ? cut.start[n + 1] : cut.end(cut.count - 1, frames);
    const uint32_t step = std::min(at - cut.start[n - 1], next > at ? next - at : 0);
    return std::max<uint32_t>(2 * hand_reach, step / 4);
}
inline Slices with_hand(const Slices& cut, const HandCuts& hand, uint32_t frames, bool* capped = nullptr) {
    if (hand.empty() || !cut.count) return cut;
    Slices s;
    s.whole(cut.start[0], cut.stop[cut.count - 1]);
    for (unsigned n = 0; n < hand.pin_count; ++n) s.add(hand.pins[n], frames);
    for (unsigned n = 1; n < cut.count; ++n) {
        if (hand.dropped(cut.start[n])) continue;
        const uint32_t at = cut.start[n], reach = give_way_reach(cut, n, frames);
        bool near_hand = false;
        for (unsigned k = 0; k < hand.pin_count && !near_hand; ++k)
            near_hand = (hand.pins[k] > at ? hand.pins[k] - at : at - hand.pins[k]) < reach;
        if (near_hand) continue;
        if (s.count >= max_slices) { if (capped) *capped = true; break; }
        s.add(cut.start[n], frames);
    }
    // Where the type leaves gaps (Auto's SHOTS and VOCAL: a slice ends where its sound does), a slice that starts
    // inside one of the type's ends no later than that one does.
    for (unsigned k = 0; k < s.count; ++k)
        for (unsigned n = 0; n < cut.count; ++n)
            if (cut.start[n] <= s.start[k] && s.start[k] < cut.end(n, frames)) {
                if (!cut.touches(n, frames) && cut.end(n, frames) < s.end(k, frames)) s.stop[k] = cut.stop[n];
                break;
            }
    return s;
}

// A chop read back from its WAV keeps its slices, not which cuts were made by hand. Against `autos` (what its type and
// amount cut now): a cut of its own with none of the type's within 20 ms (Zero Snap moves cuts a little) was made or
// moved by hand and is pinned again; one of the type's with none of its own near, and no cut of its own it gave way
// to, was taken out by hand. A Manual chop's type cuts nothing: every cut by hand.
inline void infer_hand(HandCuts& hand, const Slices& kept, const Slices& autos, uint32_t frames) {
    auto nearest = [](const Slices& s, uint32_t at) {
        uint32_t best = UINT32_MAX;
        for (unsigned n = 1; n < s.count; ++n) best = std::min(best, s.start[n] > at ? s.start[n] - at : at - s.start[n]);
        return best;
    };
    for (unsigned n = 1; n < kept.count; ++n)
        if (nearest(autos, kept.start[n]) > 2 * hand_reach) hand.pin(kept.start[n]);
    for (unsigned n = 1; n < autos.count; ++n) {
        const uint32_t d = nearest(kept, autos.start[n]);
        if (d > 2 * hand_reach && d >= give_way_reach(autos, n, frames)) hand.take_out(autos.start[n]);
    }
}

// TO PADS' playback, as Simpler's Playback: POLY the slices overlap (as before), MONO they share a mute group so one
// plays at a time, THRU that too and each plays on to the region's end. KIT lays one pad a sound (kit_sounds()).
// TO PADS asks, starting on the last one.
enum class SlicePlay : uint8_t { Poly, Mono, Thru, Kit, Count };
inline const char* slice_play_name(SlicePlay p) {
    static const char* names[] {"POLY", "MONO", "THRU", "KIT"};
    return names[unsigned(p) % unsigned(SlicePlay::Count)];
}

// KIT: one pad a sound. A slice's sound is its first hit's (engine/auto_chop.h HitSound, the hit within 30 ms of
// its start): a kick has more low than mid and high, a hat more high than mid (by 5 dB) and low, a snare more mid
// than low; anything else, or a slice with no hit there, is other. A hat that rings 150 ms or more is an open hat.
// Slices of one kind that sound alike (their bands within 6 dB of the same balance, ringing about as long) are one
// sound, and the longest of them (the most of its own tail) goes on its pad. Kicks first, then snares, hats, open
// hats and the rest; each kind's sounds the most heard first.
enum class KitKind : uint8_t { Kick, Snare, Hat, OpenHat, Other };
struct KitSound { unsigned slice = 0; KitKind kind = KitKind::Other; unsigned count = 0; };
inline KitKind kit_kind(const HitSound& h) {
    const int low = h.low, mid = h.mid, high = h.high;
    if (low >= std::max(mid, high) && low > 0) return KitKind::Kick;
    if (high >= mid + 20 && high > low) return h.ring >= 15 ? KitKind::OpenHat : KitKind::Hat;
    if (mid >= low && mid > 0) return KitKind::Snare;
    return KitKind::Other;
}
inline std::vector<KitSound> kit_sounds(const Slices& s, uint32_t frames, const std::array<Hit, max_hits>& hits, unsigned hit_count,
                                        const AutoRead& read) {
    struct Group { KitSound sound; HitSound like; bool known; };
    std::vector<Group> groups;
    for (unsigned n = 0; n < s.count; ++n) {
        int hit = -1;
        uint32_t gap = 1324;
        for (unsigned k = 0; k < hit_count && k < max_hits && k < read.sounds.size(); ++k) {
            const uint32_t d = hits[k].frame > s.start[n] ? hits[k].frame - s.start[n] : s.start[n] - hits[k].frame;
            if (d < gap) { gap = d; hit = int(k); }
        }
        const bool known = hit >= 0;
        const HitSound h = known ? read.sounds[size_t(hit)] : HitSound {};
        const KitKind kind = known ? kit_kind(h) : KitKind::Other;
        auto alike = [&](const Group& g) {
            if (!known || !g.known || g.sound.kind != kind) return false;
            const int low = (h.low - h.mid) - (g.like.low - g.like.mid), high = (h.high - h.mid) - (g.like.high - g.like.mid);
            return std::abs(low) <= 25 && std::abs(high) <= 25 && std::abs(int(h.ring) - int(g.like.ring)) <= std::max(3, int(g.like.ring) / 2);
        };
        auto it = std::find_if(groups.begin(), groups.end(), alike);
        if (it == groups.end()) { groups.push_back({KitSound {n, kind, 1}, h, known}); continue; }
        ++it->sound.count;
        const unsigned kept = it->sound.slice;
        if (s.end(n, frames) - s.start[n] > s.end(kept, frames) - s.start[kept]) it->sound.slice = n;
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return a.sound.kind != b.sound.kind ? a.sound.kind < b.sound.kind : a.sound.count > b.sound.count;
    });
    std::vector<KitSound> out;
    for (const auto& g : groups) out.push_back(g.sound);
    return out;
}
// Slice n's pad, from the chopped pad's sound, as `play` lays it; `group` the mute group MONO and THRU share.
inline PadSound slice_pad(const PadSound& parent, const Slices& s, unsigned n, uint32_t frames, SlicePlay play, uint8_t group) {
    PadSound one = slice_sound(parent, s.start[n], play == SlicePlay::Thru ? s.end(s.count - 1, frames) : s.end(n, frames));
    if (play != SlicePlay::Poly) one.group = group;
    return one;
}
// The mute group MONO and THRU put the slices in: the chopped pad's own, else the first none of the track's pads
// holds (16 when every one is taken).
template <class Pads> inline uint8_t slice_group(const PadSound& parent, const Pads& pads) {
    if (parent.group) return parent.group;
    uint32_t taken = 0;
    for (const auto& p : pads) if (p.used() && p.group) taken |= 1u << (p.group % 32);
    for (uint8_t g = 1; g <= 16; ++g) if (!(taken & (1u << g))) return g;
    return 16;
}

// The whole sample, as before parity phase 5.
inline Slices chop(ChopMode mode, int amount, uint32_t frames, const std::array<Hit, max_hits>& hits, unsigned hit_count, unsigned tempo_tenths,
                   const BeatGrid* grid = nullptr) {
    return chop(mode, amount, 0, frames, frames, hits, hit_count, tempo_tenths, grid);
}

} // namespace melodee
