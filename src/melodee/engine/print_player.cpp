// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/print_player.h"
#include <algorithm>
#include <cmath>

namespace melodee {

namespace {
// The pager's look-ahead is two chunks times its speed: a speed of rate / chunk holds about a second of stereo ahead.
float ahead_speed(float rate) { return std::max(1.0f, rate / float(stream_chunk)); }
} // namespace

void PrintClip::set_sends(uint8_t bits) {
    sends = uint8_t(bits & 15);
    stride = 2;
    for (unsigned b = 0; b < 4; ++b) stride = uint8_t(stride + ((sends >> b) & 1 ? 2 : 0));
}

uint32_t PrintClip::safe_from(uint32_t frame) const {
    if (resident) return frame < frames ? stride * (frames - frame) : 0;
    const uint64_t at = uint64_t(frame) * stride;
    for (const auto& h : source->held)
        if (at >= h.from && at < h.to) return uint32_t(h.to - at);
    return 0;
}

void PrintClip::hold_whole(std::vector<float> interleaved, uint32_t at_rate, uint32_t pass_frames, PrintTail how, uint8_t with_sends) {
    set_sends(with_sends);
    auto& part = source->part;
    source->owned = std::move(interleaved);
    frames = uint32_t(source->owned.size() / stride);
    part.left = part.right = source->owned.data();
    part.total = stride * frames;
    part.frames.store(part.total);
    rate = at_rate; pass = pass_frames; tail = how;
    resident = true;
    prepare_playback(pass);
}

uint32_t PrintClip::tail_from() const { return pass > PrintPlayer::slack ? pass - PrintPlayer::slack : 0; }

void PrintClip::prepare_playback(uint32_t shortest_pass, StreamPager* pager) {
    windows.reset();
    spare = nullptr;
    tail_voices.clear();
    if (tail == PrintTail::Ring && pass && shortest_pass && frames > tail_from()) {
        const uint32_t length = frames - tail_from();
        // Leave one voice for a fade at a launch. Tick rounding can shorten a pass by a frame.
        if (length > uint64_t(PrintPlayer::voice_count - 2) * std::max(1u, shortest_pass - 1)) {
            tail_voices.resize(size_t(length / std::max(1u, pass - 1)) + 2);
            for (auto& tail_voice : tail_voices) { tail_voice.next = spare; spare = &tail_voice; }
        }
    }
    if (pager) {
        const float speed = ahead_speed(float(rate)) * stride / 2;
        windows = std::make_unique<StreamPager::Windows>(*pager, source->part, tail_voices.size() + PrintPlayer::voice_count, speed);
    }
}

namespace {
void recycle(PrintTailVoice*& link, PrintClip& clip) {
    auto* finished = link;
    link = finished->next;
    finished->next = clip.spare;
    clip.spare = finished;
}
} // namespace

void PrintPlayer::close(Voice& v) {
    if (v.stream >= 0) {
        if (v.clip->windows) v.clip->windows->close(v.stream);
        else pager->close(v.stream);
    }
    v = Voice {};
}

void PrintPlayer::cut_all() {
    native_playback = false;
    for (auto& v : voices) close(v);
    while (ringing) {
        auto* clip = ringing->voice.clip;
        close(ringing->voice);
        recycle(ringing, *clip);
    }
}

void PrintPlayer::fade_all(float rate) {
    auto fade = [&](Voice& v) {
        v.leaving = true;
        v.step = std::min(v.step, -1.0f / (fade_seconds * rate));
    };
    for (auto& v : voices) if (v.clip) fade(v);
    for (auto* t = ringing; t; t = t->next) fade(t->voice);
}

bool PrintPlayer::leaving() const {
    for (auto* t = ringing; t; t = t->next) if (t->voice.leaving) return true;
    for (const auto& v : voices) if (v.clip && v.leaving) return true;
    return false;
}

bool PrintPlayer::sounding() const {
    if (ringing) return true;
    for (const auto& v : voices) if (v.clip) return true;
    return false;
}

void PrintPlayer::begin(PrintClip* clip, uint32_t frame, bool fade_in, float rate) {
    auto& m = voices[0];
    if (m.clip) close(m);
    if (!clip || frame >= clip->frames) return;
    if (!open_voice(m, *clip, frame, fade_in, rate)) ++underruns; // follow() can try again on the next tick
}

bool PrintPlayer::open_voice(Voice& voice, PrintClip& clip, uint32_t frame, bool fade_in, float rate) {
    int stream = -1;
    uint32_t safe = 0;
    if (!clip.resident) {
        safe = clip.safe_from(frame);
        // The pager counts interleaved floats, including the bus channels.
        const uint32_t start = clip.stride * frame;
        if (clip.windows) stream = clip.windows->open(start, safe);
        else if (pager) stream = pager->open(clip.source->part, start, false, ahead_speed(rate) * clip.stride / 2, safe);
        if (stream < 0) return false;
    }
    voice = Voice {};
    voice.clip = &clip;
    voice.at = frame;
    voice.stream = stream;
    voice.seeking = !clip.resident && safe == 0;
    voice.gain = fade_in ? 0.0f : 1.0f;
    voice.step = fade_in ? 1.0f / (fade_seconds * rate) : 0.0f;
    return true;
}

void PrintPlayer::retire_main(bool ring, float rate) {
    auto& m = voices[0];
    if (!m.clip) return;
    if (ring && m.clip->spare && m.at >= m.clip->tail_from()) {
        auto* t = m.clip->spare;
        m.clip->spare = t->next;
        t->voice = m; // transfer its open stream and current read position without a gap at the seam
        t->next = ringing;
        ringing = t;
        m = Voice {};
        return;
    }
    Voice* to = nullptr;
    for (unsigned n = 1; n < voice_count && !to; ++n) if (!voices[n].clip) to = &voices[n];
    if (!to) { // none free: the one nearest its end makes room
        to = &voices[1];
        for (unsigned n = 2; n < voice_count; ++n)
            if (voices[n].clip->frames - voices[n].at < to->clip->frames - to->at) to = &voices[n];
        close(*to);
    }
    *to = m;
    if (!ring) to->step = std::min(to->step, -1.0f / (fade_seconds * rate));
    m = Voice {};
}

void PrintPlayer::release(float rate) {
    native_playback = false;
    auto& m = voices[0];
    if (!m.clip) return;
    const PrintClip& c = *m.clip;
    // A Loop or Cut print let go where its pass ends (a launch on the bar it ends on): it stops there. Past that
    // seam a Loop print holds the pattern's own top again, its first hit with it.
    if (c.tail != PrintTail::Ring && (m.at <= slack || m.at + slack >= c.frames)) { close(m); return; }
    bool ring = c.tail == PrintTail::Ring && m.at + slack >= c.pass; // its tail is what follows the pass
    retire_main(ring, rate);
}

void PrintPlayer::follow(PrintClip* clip, uint32_t at, double start, double tick, float rate, bool native) {
    auto& m = voices[0];
    if (!clip) { release(rate); return; }
    if (m.clip != clip) { // a launch, a follow action, a song clip, Play, or the freeze itself
        const bool crossfade = clip->replaces_live && !(m.clip && m.clip->replaces_live);
        release(rate);
        const uint64_t frame = at ? uint64_t(std::max(0.0, std::ceil(start - 1e-6))) : 0;
        begin(clip, uint32_t(std::min<uint64_t>(frame, UINT32_MAX)), crossfade || at != 0, rate);
        expect = frame;
        carry = start - double(frame);
    } else if (native) {
        native_playback = true;
        return;
    } else if (native_playback) {
        // Rejoin at the permanently pinned head. Leave the current voice alone until the new cursor is available.
        if (at) return;
        if (m.at != 0) {
            Voice next;
            const bool fade = m.at < clip->frames;
            if (!clip->safe_from(0) || !open_voice(next, *clip, 0, fade, rate)) return;
            if (fade) retire_main(false, rate);
            else close(m);
            m = next;
        }
        expect = 0;
        carry = 0;
    } else if (at == 0) { // the pattern comes round
        if (clip->tail == PrintTail::Loop) {
            // The print wraps itself as the pass ends; a tick a frame or two off leaves it where it is.
            bool near_top = m.at <= slack, near_end = m.at + slack >= clip->frames;
            if (near_end && !near_top) { begin(clip, 0, false, rate); } // the last few frames of the pass skipped
            else if (!near_top) { retire_main(false, rate); begin(clip, 0, false, rate); } // started again from the top
        } else { // Ring rings its tail on in another voice; Cut has none
            retire_main(clip->tail == PrintTail::Ring && m.at + slack >= clip->pass, rate);
            begin(clip, 0, false, rate);
        }
        expect = 0;
        carry = 0;
    } else if (m.at + slack < expect || m.at > expect + slack) { // off its place (a Locate, MIDI clock in): moved there
        retire_main(false, rate);
        begin(clip, uint32_t(std::min<uint64_t>(expect, UINT32_MAX)), true, rate);
    }
    native_playback = native;
    if (native) return;
    carry += tick; // as render() places the next tick: the frames up to it, rounded up
    double run = std::ceil(carry);
    expect += uint64_t(std::max(0.0, run));
    carry -= run;
}

void PrintPlayer::render(float* left, float* right, unsigned frames, float* const* send_l, float* const* send_r,
                         float* coverage) {
    fed = 0;
    auto render_voice = [&](Voice& v, unsigned from_frame, unsigned end_frame) {
        const PrintClip& c = *v.clip;
        if (!c.resident && !c.windows && pager->dead(v.stream)) {
            ++underruns;
            close(v);
            return;
        }
        uint32_t limit = c.source->part.total;
        if (!c.resident) limit = c.windows ? c.windows->limit(v.stream) : pager->limit(v.stream);
        bool late = false, waited = false;
        unsigned f = from_frame;
        while (f < end_frame) {
            if (v.at >= c.frames) {
                // A replacement Loop still has samples for the handoff when Unfreeze lands at its seam.
                const bool loop = c.tail == PrintTail::Loop && (!v.leaving || c.replaces_live);
                if (&v == &voices[0] && (loop || (!v.leaving && native_playback))) {
                    v.at = 0;
                    if (!c.resident) {
                        uint32_t safe = c.safe_from(0);
                        if (c.windows) c.windows->restart(v.stream, 0, safe);
                        else pager->restart(v.stream, 0, safe);
                        v.seeking = safe == 0;
                        limit = c.windows ? c.windows->limit(v.stream) : pager->limit(v.stream);
                    }
                } else {
                    close(v);
                    break;
                }
            }
            const unsigned run = unsigned(std::min<uint64_t>(end_frame - f, c.frames - v.at));
            const unsigned s = c.stride;
            const uint64_t from = uint64_t(v.at) * s;
            const unsigned ready = limit > from ? unsigned(std::min<uint64_t>(run, (limit - from) / s)) : 0;
            if (ready) v.seeking = false;
            const float* d = c.data() + from;
            const float g0 = v.gain, st = v.step; // the fade as it runs here (the print's loop may end it)
            float* replaced = c.replaces_live ? coverage : nullptr;
            if (v.step == 0 && v.gain == 1) {
                for (unsigned k = 0; k < ready; ++k) { left[f + k] += d[s * k]; right[f + k] += d[s * k + 1]; }
                if (replaced) std::fill_n(replaced + f, ready, 1.0f);
            } else {
                for (unsigned k = 0; k < run; ++k) {
                    v.gain = std::clamp(v.gain + v.step, 0.0f, 1.0f);
                    if (k < ready) { left[f + k] += d[s * k] * v.gain; right[f + k] += d[s * k + 1] * v.gain; }
                }
                if (replaced) {
                    float gain = g0;
                    for (unsigned k = 0; k < ready; ++k) {
                        gain = std::clamp(gain + st, 0.0f, 1.0f);
                        replaced[f + k] = std::max(replaced[f + k], gain);
                    }
                }
                if (v.step > 0 && v.gain >= 1) v.step = 0;
            }
            // The pad sends beside it, through the same fade: each bus's pair into that bus.
            if (c.sends && send_l && send_r && ready) {
                unsigned pair = 2;
                for (unsigned b = 0; b < 4; ++b) {
                    if (!((c.sends >> b) & 1)) continue;
                    if (send_l[b] && send_r[b]) {
                        float g = g0;
                        for (unsigned k = 0; k < ready; ++k) {
                            g = st == 0 && g0 == 1 ? 1.0f : std::clamp(g + st, 0.0f, 1.0f);
                            send_l[b][f + k] += d[s * k + pair] * g; send_r[b][f + k] += d[s * k + pair + 1] * g;
                        }
                        fed |= 1u << b;
                    }
                    pair += 2;
                }
            }
            if (ready < run) {
                if (v.seeking && !(c.windows && c.windows->blocked(v.stream))) waited = true;
                else late = true;
            }
            v.at += run;
            f += run;
            if (v.step < 0 && v.gain <= 0) { close(v); break; }
        }
        underruns += late;
        waits += waited;
        if (v.clip && v.stream >= 0) {
            const uint32_t at = v.at * c.stride; // at most c.frames
            if (c.windows) c.windows->place(v.stream, at);
            else pager->place(v.stream, at);
        }
    };
    auto render_all = [&](unsigned from, unsigned end) {
        for (auto& v : voices) if (v.clip) render_voice(v, from, end);
        for (auto** link = &ringing; *link;) {
            auto& v = (*link)->voice;
            auto* clip = v.clip;
            render_voice(v, from, end);
            if (v.clip) link = &(*link)->next;
            else recycle(*link, *clip);
        }
    };
    auto& main = voices[0];
    auto* clip = main.clip;
    if (!native_playback || !clip || main.leaving || clip->tail != PrintTail::Ring || !clip->pass || clip->pass > clip->frames) {
        render_all(0, frames);
        return;
    }
    // Ring starts each native pass on its own sample boundary while its earlier tails continue at the same speed.
    unsigned from = 0;
    while (from < frames && main.clip) {
        if (main.at >= clip->pass) {
            retire_main(true, float(clip->rate));
            begin(clip, 0, false, float(clip->rate));
            if (!main.clip) break;
        }
        const unsigned end = from + std::min(frames - from, clip->pass - main.at);
        render_all(from, end);
        from = end;
    }
    if (from < frames) render_all(from, frames);
}

} // namespace melodee
