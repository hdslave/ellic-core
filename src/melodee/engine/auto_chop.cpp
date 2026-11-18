// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/auto_chop.h"
#include <algorithm>
#include <cmath>

// Auto chop's reading of a sample (read_auto) and its cuts (auto_slices).
//
// The kind. Sounds parted by silence are SHOTS. Otherwise the spectrum is split into what holds still over time
// (held notes: a band's middle level over 170 ms) and what spreads across the bands at one moment (hits: a frame's
// middle level over 17 semitones), as Fitzgerald's median filtering (2010) does it. With a steady beat (BeatGrid)
// the sample is DRUMS where the hits hold enough of its sound, else MUSIC. Held sound in a voice's range, parted
// by gaps and with few hits, is VOCAL. Anything else is HITS.
namespace melodee {

const char* sound_kind_name(SoundKind kind) {
    static const char* names[] {"", "DRUMS", "MUSIC", "VOCAL", "SHOTS", "HITS"};
    return names[unsigned(kind) % unsigned(SoundKind::Count)];
}

namespace {

constexpr uint32_t hop = 441;        // the spectrum's frames and the level's windows: 10 ms
constexpr float drums_share = 0.30f; // the hits' share of a steady sample's sound from which it is DRUMS
constexpr float vocal_hits = 0.25f;  // a voice: the hits' share at most
constexpr float vocal_range = 0.80f; // and its held sound's share between 150 Hz and 4 kHz at least
constexpr unsigned median_reach = 8; // the median filters' reach either side: 80 ms, 8 semitones

// A band level (BandFrames) as an amplitude over the floor, and back.
struct Amps {
    float a[256];
    Amps() { for (int q = 0; q < 256; ++q) a[q] = std::pow(10.0f, float(q) / 85.0f) - 1.0f; }
};
const Amps& amps() { static const Amps table; return table; }
uint8_t level_of(float amplitude) { return uint8_t(std::clamp(std::lround(85.0f * std::log10(1.0f + amplitude)), 0L, 255L)); }

// Each 10 ms window's level in dB.
std::vector<float> window_db(const std::vector<float>& mono) {
    std::vector<float> db(mono.size() / hop);
    for (size_t w = 0; w < db.size(); ++w) {
        double sum = 0;
        for (size_t n = w * hop; n < (w + 1) * hop; ++n) sum += double(mono[n]) * double(mono[n]);
        db[w] = float(10 * std::log10(sum / hop + 1e-12));
    }
    return db;
}

// The stretches louder than `below` dB under the loudest window, parted by `gap` windows or more under it. One under
// 20 ms is left out (a click).
std::vector<Span> spans_of(const std::vector<float>& db, float below, unsigned gap) {
    std::vector<Span> out;
    if (db.empty()) return out;
    const float floor = *std::max_element(db.begin(), db.end()) - below;
    bool in = false;
    unsigned quiet = 0;
    Span now;
    auto close = [&] { if (now.to - now.from >= min_slice) out.push_back(now); in = false; };
    for (size_t w = 0; w < db.size(); ++w) {
        if (db[w] > floor) {
            if (!in) { now.from = uint32_t(w * hop); in = true; }
            quiet = 0;
            now.to = uint32_t((w + 1) * hop);
        } else if (in && ++quiet >= gap) close();
    }
    if (in) close();
    return out;
}

int pitch_class(float hz) {
    const int midi = int(std::lround(69 + 12 * std::log2(std::max(hz, 1.0f) / 440.0f)));
    return ((midi % 12) + 12) % 12;
}

float cosine(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    float ab = 0, aa = 0, bb = 0;
    for (int k = 0; k < 12; ++k) { ab += a[size_t(k)] * b[size_t(k)]; aa += a[size_t(k)] * a[size_t(k)]; bb += b[size_t(k)] * b[size_t(k)]; }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 1.0f;
}

} // namespace

void read_auto(const std::vector<float>& mono, const BandFrames& spectrum, const std::array<Hit, max_hits>& hits,
               unsigned hit_count, const BeatGrid& beats, AutoRead& out) {
    out = AutoRead {};
    const auto frames = uint32_t(mono.size());
    const unsigned width = spectrum.width, count = spectrum.count;
    hit_count = std::min(hit_count, unsigned(max_hits));
    out.sounds.assign(hit_count, HitSound {});
    if (!frames) return;

    // Sounds between silences, and phrases between gaps. A sound starts at its hit where one was found at its start.
    const auto db = window_db(mono);
    out.spans = spans_of(db, 50, 3);
    out.phrases = spans_of(db, 35, 12);
    for (auto* list : {&out.spans, &out.phrases}) {
        // A stretch with no attack at its start is the tail of the sound before, rising over the line again: one
        // with it.
        std::vector<Span> joined;
        for (auto sp : *list) {
            bool attack = false;
            for (unsigned n = 0; n < hit_count && !attack; ++n)
                if (hits[n].frame + 2 * hop >= sp.from && hits[n].frame <= sp.from + 3 * hop) {
                    attack = true;
                    sp.from = std::min(sp.from, hits[n].frame);
                }
            if (!attack && !joined.empty()) joined.back().to = sp.to;
            else joined.push_back(sp);
        }
        for (size_t k = 0; k < joined.size(); ++k) {
            const uint32_t next = k + 1 < joined.size() ? joined[k + 1].from : frames;
            joined[k].to = std::min({joined[k].to + hop, next, frames}); // its last 10 ms window's tail
        }
        *list = std::move(joined);
    }

    // Held and struck, cell by cell; the loud frames' shares; each frame's notes (pitch classes, from held sound).
    const auto& a = amps().a;
    std::vector<uint8_t> total(count, 0);
    std::vector<std::array<float, 12>> notes(count, std::array<float, 12> {});
    if (width && count && spectrum.level.size() >= size_t(width) * count) {
        const uint8_t* level = spectrum.level.data();
        for (unsigned t = 0; t < count; ++t) {
            float sum = 0;
            for (unsigned b = 0; b < width; ++b) sum += a[level[t * width + b]];
            total[t] = level_of(sum);
        }
        const uint8_t loudest = *std::max_element(total.begin(), total.end());
        const unsigned stride = std::max(1u, count / 12000); // a long sample: every so many frames (2 minutes' worth)
        std::vector<int> band_pc(width, -1);
        for (unsigned b = 0; b < width; ++b) if (spectrum.hz[b] >= 150 && spectrum.hz[b] <= 5000) band_pc[b] = pitch_class(spectrum.hz[b]);
        double struck_sum = 0, all_sum = 0, held_sum = 0, range_sum = 0;
        uint8_t window[2 * median_reach + 1];
        for (unsigned t = 0; t < count; t += stride) {
            const bool loud = total[t] + 170 >= loudest; // within 40 dB of the loudest frame
            for (unsigned b = 0; b < width; ++b) {
                unsigned n = 0;
                for (unsigned u = t > median_reach ? t - median_reach : 0; u <= std::min(count - 1, t + median_reach); ++u) window[n++] = level[u * width + b];
                std::nth_element(window, window + n / 2, window + n);
                const uint8_t held = window[n / 2];
                n = 0;
                for (unsigned c = b > median_reach ? b - median_reach : 0; c <= std::min(width - 1, b + median_reach); ++c) window[n++] = level[t * width + c];
                std::nth_element(window, window + n / 2, window + n);
                const uint8_t struck = window[n / 2];
                const float e = a[level[t * width + b]] * a[level[t * width + b]];
                if (held >= struck && band_pc[b] >= 0) notes[t][size_t(band_pc[b])] += a[held] * a[held];
                if (!loud) continue;
                all_sum += e;
                if (struck > held) struck_sum += e;
                else {
                    held_sum += e;
                    if (spectrum.hz[b] >= 150 && spectrum.hz[b] < 4000) range_sum += e;
                }
            }
        }
        // The strongest held pitch of each loud frame, and how often it moves by one semitone.
        unsigned slides = 0, moves = 0;
        int before = -1;
        for (unsigned t = 0; t < count; t += stride) {
            if (total[t] + 170 < loudest) { before = -1; continue; }
            int top = -1;
            float most = 0;
            for (unsigned b = 0; b < width; ++b) {
                if (spectrum.hz[b] < 150 || spectrum.hz[b] > 1500) continue;
                const float v = a[level[t * width + b]];
                if (v > most) { most = v; top = int(b); }
            }
            if (top >= 0 && before >= 0) { ++moves; slides += std::abs(top - before) == 1; }
            before = top;
        }
        out.slide = moves ? float(slides) / float(moves) : 0.0f;
        out.percussive = all_sum > 0 ? float(struck_sum / all_sum) : 0.0f;
        out.voice = held_sum > 0 ? float(range_sum / held_sum) : 0.0f;
    }

    // Each hit's sound: its three bands at their highest in the 30 ms after it, and how long it rings.
    for (unsigned n = 0; n < hit_count && width && count; ++n) {
        const unsigned t0 = hits[n].frame / hop;
        const unsigned next = n + 1 < hit_count ? hits[n + 1].frame / hop : count;
        float low = 0, mid = 0, high = 0;
        uint8_t peak = 0;
        for (unsigned t = t0 + 1; t <= t0 + 3 && t < count; ++t) {
            float l = 0, m = 0, h = 0;
            for (unsigned b = 0; b < width; ++b) {
                const float v = a[spectrum.level[t * width + b]];
                (spectrum.hz[b] < 150 ? l : spectrum.hz[b] < 2500 ? m : h) += v;
            }
            low = std::max(low, l); mid = std::max(mid, m); high = std::max(high, h);
            peak = std::max(peak, total[t]);
        }
        auto& s = out.sounds[n];
        s.low = level_of(low); s.mid = level_of(mid); s.high = level_of(high);
        unsigned t = t0 + 1;
        while (t < count && t < next && t < t0 + 255 && total[t] + 85 >= peak) ++t;
        s.ring = uint8_t(std::min(255u, t - t0));
    }

    // How far the notes change at each hit: the 400 ms before it against the 400 ms after its attack.
    out.change.assign(hit_count, 0.0f);
    for (unsigned n = 0; n < hit_count && count; ++n) {
        const unsigned t0 = hits[n].frame / hop, reach = 40;
        std::array<float, 12> before {}, after {};
        for (unsigned t = t0 > reach ? t0 - reach : 0; t < t0 && t < count; ++t) for (size_t k = 0; k < 12; ++k) before[k] += notes[t][k];
        for (unsigned t = t0 + 3; t < t0 + 3 + reach && t < count; ++t) for (size_t k = 0; k < 12; ++k) after[k] += notes[t][k];
        out.change[n] = std::clamp(1.0f - cosine(before, after), 0.0f, 1.0f);
    }

    // The kind.
    bool shots = false;
    if (out.spans.size() >= 2) {
        uint64_t silent = 0;
        for (size_t k = 1; k < out.spans.size(); ++k) silent += out.spans[k].from - out.spans[k - 1].to;
        const uint64_t whole = out.spans.back().to - out.spans.front().from;
        // A row of single sounds, not a groove with rests: two hits a sound at most (most of them).
        std::vector<unsigned> per;
        for (const auto& sp : out.spans) {
            unsigned c = 0;
            for (unsigned n = 0; n < hit_count; ++n) if (hits[n].frame >= sp.from && hits[n].frame < sp.to && hits[n].strength >= 64) ++c;
            per.push_back(c);
        }
        std::nth_element(per.begin(), per.begin() + std::ptrdiff_t(per.size() / 2), per.end());
        shots = silent * 100 >= whole * 15 && per[per.size() / 2] <= 2;
    }
    const bool steady = beats.found() && frames >= 66150 && beats.pulse_to > beats.pulse_from
                     && uint64_t(beats.pulse_to - beats.pulse_from) * 10 >= uint64_t(frames) * 6;
    if (shots && steady && out.spans.size() >= 4) {
        // A groove with rests, not a row of sounds: most of its sounds come again a beat, two, a bar or two bars
        // later (within 25 ms), as a pattern does.
        const double beat = 44100.0 * 600 / beats.tempo;
        for (double period : {beat, 2 * beat, 4 * beat, 8 * beat}) {
            unsigned partnered = 0;
            for (const auto& sp : out.spans) {
                bool found = false;
                for (const auto& other : out.spans)
                    for (double way : {-1.0, 1.0})
                        found |= std::fabs(double(other.from) - double(sp.from) - way * period) <= 1102;
                partnered += found;
            }
            if (partnered * 10 >= out.spans.size() * 6) { shots = false; break; }
        }
    }
    const bool voice = out.phrases.size() >= 2 && out.percussive < vocal_hits && out.voice >= vocal_range;
    out.found = shots ? SoundKind::Shots
              : voice ? SoundKind::Vocal
              : steady ? (out.percussive >= drums_share ? SoundKind::Drums : SoundKind::Music)
              : SoundKind::Hits;
    // VOCAL is cut as HITS until it scores well (tools/auto_chop_score.cpp: these measures do not tell a voice from
    // keys, piano or guitar).
    out.kind = out.found == SoundKind::Vocal ? SoundKind::Hits : out.found;
}

namespace {

struct Candidate { uint32_t at; float score; };

// `want` - 1 cuts from `cands` in (from, to): the best of score times room, where room is how far it is from the
// cuts already made and the region's ends, against an even share of the region (so they spread out); never two
// within 40 ms.
std::vector<uint32_t> pick(std::vector<Candidate> cands, unsigned want, uint32_t from, uint32_t to) {
    std::vector<uint32_t> cuts;
    if (want < 2 || to <= from) return cuts;
    const double share = double(to - from) / want;
    std::vector<bool> used(cands.size(), false);
    while (cuts.size() + 1 < want) {
        int best = -1;
        double value = 0;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (used[i]) continue;
            const uint32_t at = cands[i].at;
            uint32_t room = std::min(at - from, to - at);
            for (uint32_t c : cuts) room = std::min(room, at > c ? at - c : c - at);
            if (room < 2 * min_slice) continue;
            const double v = cands[i].score * std::min(1.0, room / share);
            if (v > value) { value = v; best = int(i); }
        }
        if (best < 0) break;
        used[size_t(best)] = true;
        cuts.push_back(cands[size_t(best)].at);
    }
    std::sort(cuts.begin(), cuts.end());
    return cuts;
}

// The hits in (from, to) as candidates. A hit within 50 ms after another is one with it (a flam, a grace note, a
// layered clap): the cut at the first, as strong as the stronger. DRUMS: a ghost note is left inside the hit before:
// a hit under a third of the strong hits' level, with more mid than high, that sounds like a strong one played softly
// (its bands within 7 dB of the same balance, ringing about as long). A quiet hat stays. A hit off the beat's
// sixteenths counts less.
std::vector<Candidate> hit_candidates(const std::array<Hit, max_hits>& hits, unsigned hit_count, uint32_t from, uint32_t to,
                                      const BeatGrid* grid, const AutoRead* read = nullptr) {
    std::vector<Candidate> c;
    std::vector<int> which; // the hit each candidate is (its sound)
    for (unsigned n = 0; n < hit_count && n < max_hits; ++n) {
        const uint32_t f = hits[n].frame;
        if (f <= from + min_slice || f + min_slice >= to) continue;
        const float score = hits[n].strength / 255.0f;
        if (!c.empty() && f - c.back().at < 2205) {
            if (score > c.back().score) { c.back().score = score; which.back() = int(n); }
            continue;
        }
        c.push_back({f, score});
        which.push_back(int(n));
    }
    if (!grid || c.empty()) return c;
    std::vector<float> scores;
    for (const auto& x : c) scores.push_back(x.score);
    std::sort(scores.begin(), scores.end());
    const float strong = scores[scores.size() * 3 / 4], ghost = strong / 3;
    auto sound = [&](size_t i) { return read && size_t(which[i]) < read->sounds.size() ? read->sounds[size_t(which[i])] : HitSound {}; };
    auto softer_twin = [&](size_t i) {
        if (!read) return true;
        const HitSound a = sound(i);
        for (size_t j = 0; j < c.size(); ++j) {
            if (c[j].score < 0.6f * strong) continue;
            const HitSound b = sound(j);
            const int low = (a.low - a.mid) - (b.low - b.mid), high = (a.high - a.mid) - (b.high - b.mid);
            const int ring = std::abs(int(a.ring) - int(b.ring));
            if (std::abs(low) <= 30 && std::abs(high) <= 30 && ring <= std::max(3, int(b.ring) / 2)) return true;
        }
        return false;
    };
    std::vector<Candidate> kept;
    const bool on_grid = grid->found();
    const double sixteenth = on_grid ? 44100.0 * 600 / grid->tempo / 4 : 0;
    for (size_t i = 0; i < c.size(); ++i) {
        auto x = c[i];
        if (x.score < ghost && sound(i).high <= sound(i).mid && softer_twin(i)) continue; // a hat (more high than mid) stays
        if (on_grid) {
            auto b = std::upper_bound(grid->at.begin(), grid->at.end(), x.at);
            const uint32_t beat = b == grid->at.begin() ? grid->first : *(b - 1);
            const double off = std::fmod(std::fabs(double(x.at) - double(beat)), sixteenth);
            if (std::min(off, sixteenth - off) > 0.35 * sixteenth) x.score *= 0.7f;
        }
        kept.push_back(x);
    }
    return kept;
}

} // namespace

Slices auto_slices(const AutoRead& read, unsigned want, uint32_t from, uint32_t to, uint32_t frames,
                   const std::array<Hit, max_hits>& hits, unsigned hit_count, const BeatGrid& beats, bool* capped) {
    Slices s;
    if (capped) *capped = false;
    if (!frames) return s;
    to = to && to <= frames ? to : frames;
    from = std::min(from, to > min_slice ? to - min_slice : 0);
    s.whole(from, to);
    want = std::clamp(want, 1u, max_slices);
    auto lay = [&](const std::vector<Span>& parts) { // slices with gaps: one a part, each ending where its part does
        Slices g;
        for (const auto& p : parts) {
            const uint32_t a = std::max(p.from, from), b = std::min(p.to, to);
            if (b < a + min_slice) continue;
            if (g.count >= max_slices) { if (capped) *capped = true; break; }
            g.start[g.count] = a; g.stop[g.count] = b == frames ? 0 : b; ++g.count;
        }
        return g.count ? g : s;
    };
    switch (read.kind) {
    case SoundKind::Shots: return lay(read.spans);
    case SoundKind::Vocal: {
        // A cut where each phrase starts; then on the strongest word starts inside the phrases, up to `want`.
        std::vector<Span> phrases;
        for (const auto& p : read.phrases) if (p.to > from && p.from < to) phrases.push_back(p);
        if (phrases.empty()) break;
        std::vector<Candidate> words;
        for (const auto& p : phrases)
            for (const auto& c : hit_candidates(hits, hit_count, p.from + 2205, p.to, nullptr)) words.push_back(c);
        std::vector<uint32_t> inside;
        if (want > phrases.size()) {
            inside = pick(words, unsigned(want - phrases.size() + 1), from, to);
        }
        std::vector<Span> parts;
        for (const auto& p : phrases) {
            uint32_t at = p.from;
            for (uint32_t c : inside) if (c > p.from && c < p.to) { parts.push_back({at, c}); at = c; }
            parts.push_back({at, p.to});
        }
        return lay(parts);
    }
    case SoundKind::Music: {
        // Where the notes change most first, then the strongest hits, up to `want`.
        std::vector<Candidate> c;
        for (unsigned n = 0; n < hit_count && n < max_hits; ++n) {
            const uint32_t f = hits[n].frame;
            if (f <= from + min_slice || f + min_slice >= to) continue;
            const float score = (n < read.change.size() ? read.change[n] : 0.0f) + 0.15f * hits[n].strength / 255.0f;
            if (!c.empty() && f - c.back().at < 2205) { c.back().score = std::max(c.back().score, score); continue; }
            c.push_back({f, score});
        }
        for (uint32_t cut : pick(c, want, from, to)) s.add(cut, frames);
        return s;
    }
    default: {
        const bool drums = read.kind == SoundKind::Drums;
        for (uint32_t cut : pick(hit_candidates(hits, hit_count, from, to, drums ? &beats : nullptr, &read), want, from, to)) s.add(cut, frames);
        return s;
    }
    }
    return s;
}

} // namespace melodee
