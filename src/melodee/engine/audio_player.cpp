// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/audio_player.h"
#include <algorithm>
#include <cmath>

namespace melodee {

const RegionClip* AudioPack::at(uint32_t tick) const {
    auto after = std::upper_bound(regions.begin(), regions.end(), tick, [](uint32_t t, const RegionClip& r) { return t < r.start; });
    if (after == regions.begin()) return nullptr;
    const RegionClip& r = *(after - 1);
    return tick < r.end ? &r : nullptr;
}

unsigned AudioPack::hold_take(std::vector<float> left, std::vector<float> right, float rate) {
    auto part = std::make_unique<ZonePart>();
    owned.push_back(std::move(left));
    const float* l = owned.back().data();
    const uint32_t frames = uint32_t(owned.back().size());
    owned.push_back(std::move(right));
    part->left = l;
    part->right = owned.back().data();
    part->rate = rate;
    part->total = frames;
    part->frames.store(frames);
    takes.push_back(std::move(part));
    return unsigned(takes.size() - 1);
}

void RegionPlayer::close(Voice& v) {
    if (v.stream >= 0 && pager) pager->close(v.stream);
    v = Voice {};
}

void RegionPlayer::cut_all() { for (auto& v : voices) close(v); }

void RegionPlayer::fade_all(float rate) {
    for (auto& v : voices) {
        if (!v.clip) continue;
        v.leaving = true;
        v.fade = std::min(v.fade, -1.0f / (fade_seconds * rate));
    }
}

bool RegionPlayer::leaving() const {
    for (const auto& v : voices) if (v.clip && v.leaving) return true;
    return false;
}

bool RegionPlayer::sounding() const {
    for (const auto& v : voices) if (v.clip) return true;
    return false;
}

void RegionPlayer::begin(const RegionClip* clip, double pos, bool fade_in, float rate) {
    auto& m = voices[0];
    if (m.clip) close(m);
    if (!clip || !clip->take || pos >= clip->frames) return;
    m.clip = clip;
    m.pos = std::max(0.0, pos);
    m.step = double(clip->take->rate) / std::max(1.0f, rate);
    m.gain = fade_in ? 0.0f : 1.0f;
    m.fade = fade_in ? 1.0f / (fade_seconds * rate) : 0.0f;
    if (clip->resident) return;
    const uint32_t into = uint32_t(m.pos);
    const uint32_t frame = clip->reverse ? clip->offset + clip->frames - 1 - into : clip->offset + into;
    // A region started at its top reads what the pack pinned at once; elsewhere it waits for the pager.
    const uint32_t safe = !clip->reverse && into < clip->held ? clip->held - into : 0;
    const float ahead = float(std::max(1.0, m.step)) * std::max(1.0f, rate / float(stream_chunk)) * 0.75f; // about a second
    m.stream = pager ? pager->open(*clip->take, frame, clip->reverse, ahead, safe) : -1;
    m.seeking = safe == 0;
    if (m.stream < 0) ++underruns; // no stream free: silent until the next start
}

void RegionPlayer::retire_main(float rate) {
    auto& m = voices[0];
    if (!m.clip) return;
    Voice* to = nullptr;
    for (unsigned n = 1; n < voice_count && !to; ++n) if (!voices[n].clip) to = &voices[n];
    if (!to) { to = &voices[1]; close(*to); } // none free: the oldest makes room
    *to = m;
    to->fade = std::min(to->fade, -1.0f / (fade_seconds * rate));
    m = Voice {};
}

void RegionPlayer::release(float rate) { retire_main(rate); }

void RegionPlayer::follow(const RegionClip* clip, double into, bool jumped, float rate) {
    auto& m = voices[0];
    if (!clip) { if (m.clip) release(rate); return; }
    const double pos = into * double(clip->take ? clip->take->rate : rate) / std::max(1.0f, rate);
    if (m.clip != clip) { // a region begins (at its top, or the song started inside it)
        if (m.clip) retire_main(rate);
        begin(clip, pos, pos > slack, rate);
    } else if (jumped && std::fabs(m.pos - pos) > slack * m.step) { // somewhere else in it now
        retire_main(rate);
        begin(clip, pos, true, rate);
    }
}

void RegionPlayer::render(float* left, float* right, unsigned frames) {
    for (unsigned n = 0; n < voice_count; ++n) {
        auto& v = voices[n];
        if (!v.clip) continue;
        const RegionClip& c = *v.clip;
        const ZonePart& part = *c.take;
        if (!c.resident && (v.stream < 0 || !pager || pager->dead(v.stream))) { // no stream, or the card went
            close(v);
            continue;
        }
        const uint32_t limit = c.resident ? part.total : pager->limit(v.stream);
        const uint32_t first = c.offset, last = c.offset + c.frames - 1; // the region's frames in the take
        const float* l = part.left;
        const float* r = part.right;
        bool late = false;
        unsigned f = 0;
        for (; f < frames; ++f) {
            if (v.pos >= c.frames) { close(v); break; }
            const double x = c.reverse ? double(last) - v.pos : double(first) + v.pos;
            const uint32_t i0 = uint32_t(x);
            const uint32_t i1 = std::min(i0 + 1, last);
            const bool ready = c.resident || (c.reverse ? i0 >= limit : i1 < limit);
            if (ready) {
                v.seeking = false;
                const float t = float(x - double(i0));
                float g = quiet ? 0.0f : c.gain * v.gain;
                if (c.fade_in && v.pos < c.fade_in) g *= float(v.pos / c.fade_in);
                const double left_in = double(c.frames) - v.pos;
                if (c.fade_out && left_in < c.fade_out) g *= float(left_in / c.fade_out);
                left[f] += (l[i0] + (l[i1] - l[i0]) * t) * g;
                right[f] += (r[i0] + (r[i1] - r[i0]) * t) * g;
            } else {
                late = true;
            }
            v.pos += v.step;
            if (v.fade != 0) {
                v.gain = std::clamp(v.gain + v.fade, 0.0f, 1.0f);
                if (v.fade > 0 && v.gain >= 1) v.fade = 0;
                if (v.fade < 0 && v.gain <= 0) { close(v); break; }
            }
        }
        if (late) (v.seeking ? waits : underruns) += 1;
        if (v.clip && v.stream >= 0 && pager) {
            const double x = c.reverse ? double(last) - v.pos : double(first) + v.pos;
            pager->place(v.stream, uint32_t(std::clamp(x, double(first), double(last))));
        }
    }
}

} // namespace melodee
