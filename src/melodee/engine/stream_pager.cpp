// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/engine/stream_pager.h"
#include "melodee/engine/sample_data.h"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace melodee {
namespace {
bool real_pin(const void* address, size_t bytes, bool lock) { return (lock ? mlock(address, bytes) : munlock(address, bytes)) == 0; }
uint32_t chunk_count(uint32_t frames) { return frames ? (frames - 1) / stream_chunk + 1 : 0; }
size_t chunk_bytes(uint32_t frames, uint32_t chunk) { return std::min<uint32_t>(stream_chunk, frames - chunk * stream_chunk) * sizeof(float); }
} // namespace

ZonePart::~ZonePart() { if (mapping) munmap(mapping, mapping_bytes); }

namespace {
// A side's room in a part read in place: whole chunks, so each chunk of each side is whole pages to let go of.
size_t side_bytes(uint32_t frames) { return (size_t(frames) * sizeof(float) + stream_chunk_bytes - 1) / stream_chunk_bytes * stream_chunk_bytes; }

// A chunk of a part read in place, from its file into the part's memory as floats. False when the file gives less.
bool fill_chunk(ZonePart& part, uint32_t chunk) {
    const ZoneSource& source = *part.source;
    const uint32_t first = chunk * stream_chunk, frames = std::min<uint32_t>(stream_chunk, part.total - first);
    WavLayout layout;
    layout.channels = source.channels; layout.bits = source.bits; layout.floating = source.floating;
    const size_t block = layout.block();
    static thread_local std::vector<uint8_t> bytes; // the pager's thread, and a landing thread's hold()
    bytes.resize(size_t(frames) * block);
    const int file = open(source.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) return false; // the card is gone
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t got = pread(file, bytes.data() + done, bytes.size() - done, off_t(source.data_at + uint64_t(first) * block + done));
        if (got <= 0) break; // the file is shorter than it said
        done += size_t(got);
    }
    close(file);
    if (done < bytes.size()) return false;
    float* left = const_cast<float*>(part.left) + first;
    wav_frames(layout, bytes.data(), frames, left, part.right != part.left ? const_cast<float*>(part.right) + first : nullptr);
    return true;
}

// The same chunk given back: its pages go, and read as silence until it is filled again.
void empty_chunk(ZonePart& part, uint32_t chunk) {
    const uint32_t first = chunk * stream_chunk;
    madvise(const_cast<float*>(part.left) + first, stream_chunk_bytes, MADV_DONTNEED);
    if (part.right != part.left) madvise(const_cast<float*>(part.right) + first, stream_chunk_bytes, MADV_DONTNEED);
}
} // namespace

bool read_frames(const ZonePart& part, uint32_t from, uint32_t frames, float* left, float* right) {
    if (!part.source || from >= part.total) return false;
    frames = std::min(frames, part.total - from);
    const ZoneSource& source = *part.source;
    WavLayout layout;
    layout.channels = source.channels; layout.bits = source.bits; layout.floating = source.floating;
    const size_t block = layout.block();
    std::vector<uint8_t> bytes(size_t(frames) * block);
    const int file = open(source.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t got = pread(file, bytes.data() + done, bytes.size() - done, off_t(source.data_at + uint64_t(from) * block + done));
        if (got <= 0) break;
        done += size_t(got);
    }
    close(file);
    if (done < bytes.size()) return false;
    wav_frames(layout, bytes.data(), frames, left, right);
    return true;
}

bool read_in_place(const std::string& path, const WavLayout& layout, ZonePart& part) {
    if (!layout.frames || layout.channels < 1 || layout.channels > 2) return false;
    if (access(path.c_str(), R_OK) != 0) return false;
    auto source = std::make_unique<ZoneSource>();
    source->path = path;
    source->data_at = layout.data_at; source->channels = uint8_t(layout.channels); source->bits = uint8_t(layout.bits);
    source->floating = layout.floating;
    // Room for every frame, none of it memory yet: a page is had only once a chunk is filled.
    const size_t side = side_bytes(layout.frames), bytes = side * layout.channels;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* start = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (start == MAP_FAILED) return false;
    part.mapping = start; part.mapping_bytes = bytes;
    part.left = static_cast<const float*>(start);
    part.right = layout.channels > 1 ? part.left + side / sizeof(float) : part.left;
    part.pins.assign((layout.frames - 1) / stream_chunk + 1, 0);
    part.rate = float(layout.rate);
    part.total = layout.frames;
    part.source = std::move(source);
    return true;
}

StreamPager::StreamPager(Pin pin) : pin_fn(pin ? pin : real_pin) {}
StreamPager::~StreamPager() { stop(); }

void StreamPager::start() {
    if (running.exchange(true)) return;
    thread = std::thread([this] { run(); });
}

void StreamPager::stop() {
    if (!running.exchange(false)) return;
    if (thread.joinable()) thread.join();
}

void StreamPager::run() {
    // Below the audio thread (30), above whatever else would keep it from the card.
    sched_param priority {};
    priority.sched_priority = 20;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority); // best effort
    while (running.load(std::memory_order_relaxed))
        if (!pump()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

int StreamPager::open(ZonePart& part, uint32_t frame, bool reverse, float speed, uint32_t safe) {
    Sending held(sending);
    for (unsigned n = 0; n < stream_count; ++n) {
        auto& s = slots[n];
        if (s.used) continue;
        s.used = true;
        s.closing = false;
        s.dead.store(false, std::memory_order_relaxed);
        s.reverse = reverse;
        s.head = head_of(frame, safe, reverse, part.total);
        s.at.store(frame, std::memory_order_relaxed);
        Message m;
        m.kind = Message::Open; m.stream = int16_t(n); m.reverse = reverse; m.part = &part; m.frame = frame; m.speed = speed;
        m.serial = ++s.serial;
        if (!messages.push(m)) { s.used = false; return -1; }
        return int(n);
    }
    return -1;
}

void StreamPager::restart(int stream, uint32_t frame, uint32_t safe) {
    Sending held(sending);
    auto& s = slots[size_t(stream)];
    s.head = head_of(frame, safe, s.reverse, UINT32_MAX);
    s.at.store(frame, std::memory_order_relaxed);
    Message m;
    m.kind = Message::Restart; m.stream = int16_t(stream); m.frame = frame; m.reverse = s.reverse;
    m.serial = ++s.serial;
    messages.push(m); // on overflow the head still plays, and a gap follows
}

uint32_t StreamPager::head_of(uint32_t frame, uint32_t safe, bool reverse, uint32_t total) {
    if (!reverse) return uint32_t(std::min<uint64_t>(uint64_t(frame) + safe, total));
    return safe ? (frame + 1 > safe ? frame + 1 - safe : 0) : UINT32_MAX; // reversed: from here down, held; none: nothing
}

uint32_t StreamPager::limit(int stream) const {
    const auto& s = slots[size_t(stream)];
    uint64_t ready = s.ready.load(std::memory_order_acquire);
    if (uint32_t(ready >> 32) == s.serial) return s.reverse ? std::min(uint32_t(ready), s.head) : std::max(uint32_t(ready), s.head);
    return s.head;
}

void StreamPager::close(int stream) {
    Sending held(sending);
    auto& s = slots[size_t(stream)];
    Message m;
    m.kind = Message::Close; m.stream = int16_t(stream);
    if (messages.push(m)) { s.used = false; s.closing = false; }
    else s.closing = true;
}

void StreamPager::block() { // close() takes the lock for each
    for (unsigned n = 0; n < stream_count; ++n) if (slots[n].closing) close(int(n));
}

void StreamPager::flush() {
    if (!running.load(std::memory_order_relaxed)) { pump(); return; }
    uint64_t asked = flush_asked.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (unsigned n = 0; n < 2000 && flush_done.load(std::memory_order_acquire) < asked; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

bool StreamPager::pin_chunk(ZonePart& part, uint32_t chunk) {
    if (part.source) { // read in place: the first to want a chunk fills it from the file
        std::lock_guard<std::mutex> guard(part.source->lock);
        if (part.pins[chunk]) { ++part.pins[chunk]; return true; }
        if (!fill_chunk(part, chunk)) return false;
        part.pins[chunk] = 1;
        return true;
    }
    if (part.pins[chunk]) { ++part.pins[chunk]; return true; }
    size_t bytes = chunk_bytes(part.total, chunk);
    if (!pin_fn(part.left + chunk * stream_chunk, bytes, true)) return false;
    if (part.right != part.left && !pin_fn(part.right + chunk * stream_chunk, bytes, true)) {
        pin_fn(part.left + chunk * stream_chunk, bytes, false);
        return false;
    }
    part.pins[chunk] = 1;
    return true;
}

void StreamPager::unpin_chunk(ZonePart& part, uint32_t chunk) {
    if (part.source) { // read in place: the last to need it gives its memory back
        std::lock_guard<std::mutex> guard(part.source->lock);
        if (--part.pins[chunk]) return;
        empty_chunk(part, chunk);
        return;
    }
    if (--part.pins[chunk]) return;
    size_t bytes = chunk_bytes(part.total, chunk);
    pin_fn(part.left + chunk * stream_chunk, bytes, false);
    if (part.right != part.left) pin_fn(part.right + chunk * stream_chunk, bytes, false);
}

bool StreamPager::hold(ZonePart& part, uint32_t frame, uint32_t frames) {
    const uint32_t total = part.total;
    if (!total || frame >= total) return true;
    uint32_t last = std::min(frame + std::max(frames, 1u), total) - 1;
    for (uint32_t chunk = frame / stream_chunk; chunk <= last / stream_chunk; ++chunk)
        if (!pin_chunk(part, chunk)) return false;
    return true;
}

void StreamPager::release(Work& w) {
    if (w.part) for (uint32_t c = w.lo; c < w.hi; ++c) unpin_chunk(*w.part, c);
    w.lo = w.hi = 0;
}

void StreamPager::begin(Work& w, const Message& m) {
    release(w);
    if (m.part) w.part = m.part;
    if (m.kind == Message::Open) w.speed = m.speed;
    w.serial = m.serial;
    w.reverse = m.reverse;
    w.dead = false;
    uint32_t chunk = m.frame / stream_chunk;
    w.lo = w.hi = w.reverse ? chunk + 1 : chunk;
}

// One more chunk ahead of the voice if it wants one, and what it has left behind let go.
bool StreamPager::step(unsigned n) {
    auto& w = work[n];
    if (!w.part || w.dead) return false;
    auto& slot = slots[n];
    const uint32_t frames = w.part->total, chunks = chunk_count(frames);
    const uint32_t at = slot.at.load(std::memory_order_relaxed);
    const uint32_t ahead = uint32_t(stream_chunk * std::max(1.0f, w.speed) * 2);
    auto publish = [&](uint32_t limit) { slot.ready.store(uint64_t(w.serial) << 32 | limit, std::memory_order_release); };
    if (!w.reverse) {
        uint32_t want = std::min<uint64_t>(chunks, (uint64_t(at) + ahead) / stream_chunk + 1);
        if (w.hi < want) {
            if (!pin_chunk(*w.part, w.hi)) { w.dead = true; slot.dead.store(true, std::memory_order_relaxed); return false; }
            ++w.hi;
            publish(std::min<uint64_t>(uint64_t(w.hi) * stream_chunk, frames));
            return true;
        }
        uint32_t chunk = at / stream_chunk, keep = chunk ? chunk - 1 : 0; // a chunk behind stays, for a stretch's taps
        while (w.lo < keep && w.lo < w.hi) unpin_chunk(*w.part, w.lo++);
    } else {
        uint32_t low = at > ahead ? (at - ahead) / stream_chunk : 0;
        if (w.lo > low) {
            if (!pin_chunk(*w.part, w.lo - 1)) { w.dead = true; slot.dead.store(true, std::memory_order_relaxed); return false; }
            --w.lo;
            publish(w.lo * stream_chunk);
            return true;
        }
        uint32_t keep = at / stream_chunk + 2;
        while (w.hi > keep && w.hi > w.lo) unpin_chunk(*w.part, --w.hi);
    }
    return false;
}

bool StreamPager::pump() {
    bool did = false;
    uint64_t asked = flush_asked.load(std::memory_order_acquire);
    Message m;
    while (messages.pop(m)) {
        did = true;
        auto& w = work[size_t(m.stream)];
        if (m.kind == Message::Close) { release(w); w.part = nullptr; }
        else begin(w, m);
    }
    flush_done.store(asked, std::memory_order_release);
    for (unsigned n = 0; n < stream_count; ++n) did |= step(n);
    return did;
}

} // namespace melodee
