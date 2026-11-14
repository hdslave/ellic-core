// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The MPC 3 export (shell/mpc3_export.h). The template is a project MPC 3
// saved (mpc3_template.json), cut down by tools/mpc3_template.py to one of each thing: a drum track,
// an empty pad, the Submix, Return and Out tracks, a sequence, a clip and a note
// event. Each export reads it afresh and fills copies. Numbers keep the kind
// the template gives them: MPC 3 reads 1.0 and 1 differently, and JUCE writes a
// double as 1.0.
#include "melodee/shell/mpc3_export.h"
#include "melodee/engine/dsp.h"
#include "melodee/model/sampler.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>

asm(R"(
    .section .rodata
    .balign 16
    .global melodee_mpc3_template_start
melodee_mpc3_template_start:
    .incbin "src/melodee/shell/mpc3_template.json"
    .global melodee_mpc3_template_end
melodee_mpc3_template_end:
    .previous
)");

extern "C" const unsigned char melodee_mpc3_template_start[], melodee_mpc3_template_end[];

namespace melodee {
namespace fs = std::filesystem;
namespace {

constexpr unsigned mpc_ticks_per_tick = 960 / ticks_per_quarter; // the MPC counts 960 a quarter
constexpr unsigned mpc_pads = 128;

juce::var read_template() {
    return juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(melodee_mpc3_template_start),
                                                    int(melodee_mpc3_template_end - melodee_mpc3_template_start)));
}

// A property of an object, to change in place (objects and arrays inside are shared, not copied).
juce::var& field(const juce::var& object, const char* key) {
    static juce::var none;
    auto* dynamic = object.getDynamicObject();
    auto* value = dynamic ? dynamic->getProperties().getVarPointer(key) : nullptr;
    if (!value) { none = juce::var(); return none; }
    return *value;
}
void put(const juce::var& object, const char* key, juce::var value) {
    if (auto* dynamic = object.getDynamicObject()) dynamic->setProperty(key, std::move(value));
}
juce::var real(double value) { return juce::var(value); } // written with a decimal point, as MPC 3 has it
juce::var whole(int value) { return juce::var(value); }
juce::var array_of(std::vector<juce::var> items) {
    juce::Array<juce::var> out;
    for (auto& item : items) out.add(std::move(item));
    return juce::var(out);
}
juce::var object() { return juce::var(new juce::DynamicObject()); }
juce::var entry(juce::var key, juce::var value) {
    auto made = object();
    put(made, "key", std::move(key));
    put(made, "value", std::move(value));
    return made;
}

// Melodee's 0..127 level (100 is 0 dB) as an MPC Volume: the inverse of mpc_level() (engine/keygroup.cpp).
double mpc_volume(unsigned level) {
    if (!level) return 0.0;
    double db = level >= 100 ? (level - 100) / 27.0 * level_top_db : 37 * std::log10(level / 100.0);
    return std::pow(10.0, (db - level_top_db) / 40);
}
double mpc_pan_of(int pan) { return 0.5 + pan / 100.0; }
double fraction(unsigned percent) { return std::clamp(percent, 0u, 100u) / 100.0; }

// The pad's filter as the MPC's filter table (the inverse of mpc_filter()).
int mpc_filter_type(PadFilter filter) {
    switch (filter) {
    case PadFilter::Lpf2: return 2;
    case PadFilter::Lpf4: return 3;
    case PadFilter::Hpf2: return 7;
    case PadFilter::Hpf4: return 8;
    case PadFilter::Bpf2: return 11;
    case PadFilter::Bpf4: return 12;
    case PadFilter::Classic: return 29;
    default: return 0;
    }
}

struct Wav {
    unsigned rate = 0;
    uint64_t frames = 0;
};
Wav wav_of(const std::string& path) {
    Wav out;
    std::ifstream file(path, std::ios::binary);
    char head[12];
    if (!file.read(head, sizeof(head)) || std::memcmp(head, "RIFF", 4) || std::memcmp(head + 8, "WAVE", 4)) return out;
    unsigned align = 0;
    char chunk[8];
    while (file.read(chunk, sizeof(chunk))) {
        const uint32_t size = juce::ByteOrder::littleEndianInt(chunk + 4);
        if (!std::memcmp(chunk, "fmt ", 4) && size >= 16) {
            char format[16];
            if (!file.read(format, sizeof(format))) return out;
            out.rate = juce::ByteOrder::littleEndianInt(format + 4);
            align = juce::ByteOrder::littleEndianShort(format + 12);
            file.seekg(std::streamoff(size - 16) + (size & 1), std::ios::cur);
            continue;
        }
        if (!std::memcmp(chunk, "data", 4)) {
            if (align) out.frames = size / align;
            return out;
        }
        file.seekg(std::streamoff(size) + (size & 1), std::ios::cur);
    }
    return out;
}

bool is_wav(const SampleRef& ref) {
    if (!ref.used() || ref.instrument()) return false;
    auto kind = fs::path(ref.path).extension().string();
    for (auto& c : kind) c = char(std::tolower(static_cast<unsigned char>(c)));
    return kind == ".wav";
}

// The project's WAV files, each copied once into the data folder under a name of its own.
class Files {
public:
    Files(const Project& p, fs::path f) : project(p), folder(std::move(f)) {}
    // The file's name in the data folder; empty when the slot is not a WAV file.
    const std::string& name(unsigned slot, Mpc3Export& report) {
        static const std::string none;
        const auto& ref = project.samples[slot % sample_slots];
        if (!is_wav(ref)) return none;
        auto found = names.find(slot);
        if (found != names.end()) return found->second;
        auto stem = fs::path(ref.path).stem().string();
        std::string file = stem + ".wav";
        for (unsigned n = 2; taken.count(file); ++n) file = stem + " " + std::to_string(n) + ".wav";
        taken.insert(file);
        std::error_code error;
        if (fs::exists(ref.path, error)) fs::copy_file(ref.path, folder / file, fs::copy_options::overwrite_existing, error);
        else ++report.missing;
        infos[slot] = wav_of((folder / file).string());
        ++report.samples;
        return names[slot] = file;
    }
    // A Melodee frame (44.1 kHz) as a frame of the slot's file.
    uint32_t frame(unsigned slot, uint32_t at) const {
        auto found = infos.find(slot);
        const double rate = found != infos.end() && found->second.rate ? found->second.rate : dsp::model_rate;
        return uint32_t(std::lround(at * rate / dsp::model_rate));
    }
    // The region's end in the file's frames: 0 is the file's end.
    uint32_t end(unsigned slot, uint32_t at) const {
        if (at) return frame(slot, at);
        auto found = infos.find(slot);
        return found != infos.end() ? uint32_t(found->second.frames) : 0;
    }
    // The MPC's entry for a file: the project's and a program's samples lists.
    static juce::var sample_entry(const std::string& file) {
        return juce::JSON::parse(juce::String("{\"version\": 1, \"name\": ") + juce::JSON::toString(juce::String(fs::path(file).stem().string())) +
                                 ", \"path\": " + juce::JSON::toString(juce::String(file)) +
                                 ", \"loadImpl\": 0, \"metadata\": {\"tempo\": 0.0, \"rootNote\": 60, \"tune\": 0.0, \"key\": \"\"}}");
    }
    std::vector<std::string> all() const {
        std::vector<std::string> out;
        for (const auto& [slot, file] : names) out.push_back(file);
        return out;
    }
private:
    const Project& project;
    fs::path folder;
    std::map<unsigned, std::string> names;
    std::map<unsigned, Wav> infos;
    std::set<std::string> taken;
};

// One layer of a pad, from a sound region of a WAV file.
void fill_layer(const juce::var& layer, const std::string& file, uint32_t start, uint32_t end, uint32_t loop_start, bool loop, bool reverse) {
    put(layer, "active", true);
    put(layer, "sampleName", juce::String(fs::path(file).stem().string()));
    put(layer, "sampleFile", juce::String(file));
    put(layer, "direction", whole(reverse ? 1 : 0));
    put(layer, "loopMode", whole(loop ? 1 : 0));
    const auto& slice = field(layer, "sliceInfo");
    put(slice, "Start", whole(int(start)));
    put(slice, "End", whole(int(end)));
    put(slice, "LoopStart", whole(int(loop_start)));
    put(slice, "LoopMode", whole(loop ? 1 : 0));
}

// A kit pad as the MPC's pad (an "instrument"); false when none of its layers is a WAV file.
bool fill_pad(const juce::var& instrument, const Track& track, unsigned n, Files& files, std::vector<std::string>& used, Mpc3Export& report) {
    const auto& pad = track.pads[n];
    const auto* layers = field(instrument, "layersv").getArray();
    if (!layers || layers->isEmpty()) return false;
    unsigned written = 0;
    for (unsigned l = 0; l < pad_layers && l < unsigned(layers->size()); ++l) {
        const auto layer_sound = pad.layer(l);
        if (!layer_sound.used()) continue;
        const auto& file = files.name(layer_sound.sample, report);
        if (file.empty()) { ++report.skipped_pads; continue; }
        const auto& layer = layers->getReference(int(written));
        const unsigned slot = layer_sound.sample;
        fill_layer(layer, file, files.frame(slot, layer_sound.start), files.end(slot, layer_sound.end),
                   files.frame(slot, pad.loop_lock ? layer_sound.start : pad.loop_start), pad.loop, pad.reverse);
        put(layer, "velocityStart", whole(l ? layer_sound.low : 0)); // the MPC's first layer starts at 0, as it saves one
        put(layer, "velocityEnd", whole(layer_sound.high));
        if (l) {
            put(layer, "coarseTune", whole(layer_sound.pitch));
            put(layer, "fineTune", whole(layer_sound.fine));
            put(layer, "pan", real(mpc_pan_of(layer_sound.pan)));
            put(field(layer, "volume"), "gainCoefficient", real(std::pow(10.0, layer_sound.gain / 2000.0)));
        } else if (pad.gain) {
            put(field(layer, "volume"), "gainCoefficient", real(std::pow(10.0, pad.gain / 2000.0)));
        }
        if (std::find(used.begin(), used.end(), file) == used.end()) used.push_back(file);
        ++written;
    }
    if (!written) return false;

    put(instrument, "coarseTune", whole(pad.pitch));
    put(instrument, "fineTune", whole(pad.fine));
    put(instrument, "monophonic", pad.mono);
    put(instrument, "polyphony", whole(pad.mono ? 1 : 0)); // MPC 3 makes a pad of polyphony 1 mono again; poly is 0, as it saves one
    put(instrument, "whichMuteGroup", whole(pad.group));
    put(instrument, "loopLock", pad.loop_lock);
    put(instrument, "triggerMode", whole(pad.play == PadPlay::OneShot ? 0 : 1));
    const auto& mix = field(instrument, "mixable");
    put(mix, "volume", real(mpc_volume(pad.level)));
    put(mix, "pan", real(mpc_pan_of(pad.pan)));
    put(mix, "mute", track.pad_muted(n));
    const auto& synth = field(instrument, "synthSection");
    put(synth, "velocitySensitivity", real(fraction(pad.velocity_sense)));
    const auto& amp = field(synth, "ampEnvelope");
    put(field(amp, "Attack"), "value0", real(fraction(pad.attack)));
    put(field(amp, "Decay"), "value0", real(fraction(pad.decay)));
    put(field(amp, "Sustain"), "value0", real(fraction(pad.sustain)));
    put(field(amp, "Release"), "value0", real(fraction(pad.release)));
    put(field(amp, "DecayFromEnd"), "value0", !pad.decay_from_start);
    put(field(amp, "OneShot"), "value0", pad.play == PadPlay::OneShot);
    const auto& filter = field(field(synth, "filterData"), "value0");
    put(filter, "filterType", whole(mpc_filter_type(pad.filter)));
    put(filter, "filterCutoff", real(fraction(pad.cutoff)));
    put(filter, "filterResonance", real(fraction(pad.resonance)));
    put(filter, "filterEnvelopeAmount", real(fraction(pad.filter_amount)));
    const auto& filter_env = field(synth, "filterEnvelope");
    put(field(filter_env, "Attack"), "value0", real(fraction(pad.filter_attack)));
    put(field(filter_env, "Decay"), "value0", real(fraction(pad.filter_decay)));
    put(field(filter_env, "Sustain"), "value0", real(fraction(pad.filter_sustain)));
    put(field(filter_env, "Release"), "value0", real(fraction(pad.filter_release)));
    return true;
}

// A name for each written track: its own, else "Drum NNN"; never one another track or a bus has.
std::string unique_name(const char* own, unsigned number, std::set<std::string>& taken) {
    char fallback[16];
    std::snprintf(fallback, sizeof(fallback), "Drum %03u", number);
    std::string base = own && own[0] ? own : fallback;
    std::string name = base;
    for (unsigned n = 2; taken.count(name); ++n) name = base + " " + std::to_string(n);
    taken.insert(name);
    return name;
}

// Every "track" a Q-Link or pad assignment targets (the template's own tracks' names) pointed at `first`, as MPC 3
// does itself on loading when the name is gone.
void retarget_tracks(const juce::var& node, const std::set<std::string>& names, const juce::String& first) {
    if (auto* object = node.getDynamicObject()) {
        bool gone = false;
        for (const auto& property : object->getProperties()) {
            if (property.name.toString() == "track" && property.value.isString()) {
                const auto target = property.value.toString().toStdString();
                gone = !target.empty() && !names.count(target); // an empty name targets no track
            } else
                retarget_tracks(property.value, names, first);
        }
        if (gone) object->setProperty("track", first);
    } else if (const auto* list = node.getArray()) {
        for (const auto& item : *list) retarget_tracks(item, names, first);
    }
}

bool kit_track_worth_writing(const Track& track, const PatternBank& patterns, unsigned t) {
    for (const auto& pad : track.pads)
        if (pad.used()) return true;
    for (const auto& pattern : patterns.at[t])
        if (!pattern.events.empty()) return true;
    return false;
}

} // namespace

std::string export_mpc3_project(const Project& project, const PatternBank& patterns, const std::string& folder,
                                const std::string& name, Mpc3Export& report) {
    report = Mpc3Export {};
    const juce::var doc = read_template();
    const auto& data = field(doc, "data");
    if (!data.isObject()) return "MPC 3 template unreadable";
    const fs::path where(folder);
    const fs::path data_folder = where / (name + "_[ProjectData]");
    std::error_code error;
    fs::create_directories(data_folder, error);
    if (error) return "Cannot make " + data_folder.string();

    Files files(project, data_folder);
    std::set<std::string> taken;
    std::vector<std::pair<std::string, juce::var>> buses; // name, track
    if (const auto* list = field(doc, "buses").getArray())
        for (const auto& bus : *list) {
            buses.emplace_back(bus["name"].toString().toStdString(), bus.clone());
            taken.insert(buses.back().first);
        }

    // Kit tracks as Drum tracks.
    struct Written { unsigned index; std::string name; juce::var track; };
    std::vector<Written> written;
    const auto& pad_notes = field(field(field(doc, "drumTrack"), "program"), "padNoteMap")["noteForPad"];
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& track = project.tracks[t];
        if (!track.used()) continue;
        if (track.engine != EngineKind::Sampler) { ++report.skipped_tracks; continue; }
        if (!kit_track_worth_writing(track, patterns, t)) continue;
        juce::var made = field(doc, "drumTrack").clone();
        const std::string track_name = unique_name(track.name, unsigned(written.size() + 1), taken);
        put(made, "name", juce::String(track_name));
        put(made, "mute", track.mix.mute);
        const auto& program = field(made, "program");
        put(program, "name", juce::String(track_name));
        const auto& mix = field(program, "mixable");
        put(mix, "volume", real(mpc_volume(track.mix.level)));
        put(mix, "pan", real(mpc_pan_of(track.mix.pan)));
        put(mix, "mute", track.mix.mute);
        put(mix, "solo", track.mix.solo);
        juce::Array<juce::var> instruments;
        std::vector<std::string> used;
        for (unsigned n = 0; n < mpc_pads; ++n) {
            juce::var instrument = field(doc, "drumPad").clone();
            if (n < sampler_pads && track.pads[n].used() && fill_pad(instrument, track, n, files, used, report)) ++report.pads;
            instruments.add(instrument);
        }
        put(field(program, "drum"), "instruments", juce::var(instruments));
        std::vector<juce::var> samples;
        for (const auto& file : used) samples.push_back(Files::sample_entry(file));
        put(program, "samples", array_of(std::move(samples)));
        written.push_back({t, track_name, made});
    }
    if (written.empty()) return "No kit tracks to export";
    report.tracks = unsigned(written.size());

    // Every name, as MPC 3 keys its clips: sorted, the buses among them.
    std::vector<std::string> names;
    for (const auto& w : written) names.push_back(w.name);
    for (const auto& b : buses) names.push_back(b.first);
    std::sort(names.begin(), names.end());

    // The pads' notes: pad n plays the MPC's note for pad n.
    auto note_of_pad = [&](unsigned pad) {
        const auto& v = pad_notes[juce::Identifier("value" + juce::String(pad))];
        return v.isVoid() ? int((sampler_first_note + pad) % 128) : int(v);
    };

    // Patterns as sequences: all 128, the highest first, as MPC 3 saves them.
    const TimeSignature signature = project.time_signature();
    const int beat_pulses = int(3840 / signature.unit);
    const int bar_pulses = beat_pulses * signature.beats;
    juce::Array<juce::var> sequences;
    for (int p = int(pattern_count) - 1; p >= 0; --p) {
        uint32_t longest = 0;
        bool any = false;
        for (const auto& w : written) {
            const auto& pattern = patterns.at[w.index][size_t(p)];
            longest = std::max(longest, pattern.length);
            any = any || !pattern.events.empty();
        }
        report.sequences += any;
        const int pulses = int(std::max<uint32_t>(longest, 1) * mpc_ticks_per_tick);
        const int bars = std::max(1, (pulses + bar_pulses - 1) / bar_pulses);
        juce::var sequence = field(doc, "sequence").clone();
        char title[24];
        std::snprintf(title, sizeof(title), "Sequence %02d", p + 1);
        put(sequence, "name", juce::String(title));
        const unsigned tempo = project.seq_tempo && project.pattern_tempo[size_t(p)] ? project.pattern_tempo[size_t(p)] : project.tempo;
        put(sequence, "bpm", real(tempo / 10.0));
        put(sequence, "lengthBars", whole(bars));
        put(sequence, "loopStartBar", whole(0));
        put(sequence, "loopEndBar", whole(bars));
        put(sequence, "lengthPulses", whole(pulses)); // a pattern of part of a bar keeps its length; the bars round up
        put(sequence, "loopStartPulses", whole(0));
        put(sequence, "loopEndPulses", whole(pulses));
        auto signature_list = [&] {
            auto one = object();
            put(one, "beatsPerBar", whole(signature.beats));
            put(one, "beatLength", whole(beat_pulses));
            put(one, "barStart", whole(0));
            auto list = object();
            put(list, "timeSignatures", array_of({one}));
            return list;
        };
        put(sequence, "timeSignatureTrack", signature_list());

        std::vector<juce::var> clips;
        for (const auto& track_name : names) {
            juce::var clip = field(doc, "clip").clone();
            put(clip, "name", juce::String(track_name));
            put(clip, "timeSignatureList", signature_list());
            int clip_pulses = pulses;
            juce::Array<juce::var> events;
            auto own = std::find_if(written.begin(), written.end(), [&](const Written& w) { return w.name == track_name; });
            if (own != written.end()) {
                const auto& pattern = patterns.at[own->index][size_t(p)];
                if (!pattern.events.empty()) clip_pulses = int(pattern.length * mpc_ticks_per_tick);
                for (const auto& e : pattern.events) {
                    if (e.key < sampler_first_note || e.key >= sampler_first_note + mpc_pads || e.tick >= pattern.length) continue;
                    juce::var event = field(doc, "noteEvent").clone();
                    put(event, "time", whole(int(e.tick * mpc_ticks_per_tick)));
                    put(event, "muted", false);
                    const auto& note = field(event, "note");
                    put(note, "note", whole(note_of_pad(e.key - sampler_first_note)));
                    put(note, "velocity", real(std::clamp<int>(e.velocity, 1, 127) / 127.0));
                    put(note, "length", whole(int(std::max<unsigned>(e.length, 1) * mpc_ticks_per_tick)));
                    put(note, "probability", whole(std::clamp<int>(e.chance, 0, 100)));
                    put(note, "ratchet", whole(int(e.hits())));
                    events.add(event);
                    ++report.notes;
                }
            }
            put(clip, "endPulses", whole(clip_pulses));
            put(clip, "loopStartPulses", whole(0));
            put(clip, "loopEndPulses", whole(clip_pulses));
            put(field(clip, "eventList"), "events", juce::var(events));
            clips.push_back(entry(juce::String(track_name), clip));
        }
        put(sequence, "trackClipMaps", array_of({array_of(std::move(clips))}));
        sequences.add(entry(whole(p), sequence));
    }

    // The project: its tracks, the buses after them, sequences, samples and tempo.
    juce::Array<juce::var> tracks;
    for (const auto& w : written) tracks.add(w.track);
    for (const auto& b : buses) tracks.add(b.second);
    put(data, "tracks", juce::var(tracks));
    put(data, "sequences", juce::var(sequences));
    std::vector<juce::var> samples;
    for (const auto& file : files.all()) samples.push_back(Files::sample_entry(file));
    put(data, "samples", array_of(std::move(samples)));
    put(data, "masterTempoEnabled", !project.seq_tempo);
    put(data, "masterTempo", real(project.tempo / 10.0));
    put(data, "currentSequence", whole(project.tracks[written.front().index].pattern % pattern_count));
    put(data, "currentTrackIndex", whole(0));
    std::vector<juce::var> transport;
    for (const auto& track_name : names) {
        auto start = object();
        put(start, "key", whole(0));
        put(start, "value", whole(0));
        transport.push_back(entry(juce::String(track_name), array_of({start})));
    }
    put(field(data, "clipPlayerData"), "trackClipTransportMap", array_of(std::move(transport)));
    put(field(data, "info"), "title", juce::String(name));
    retarget_tracks(data, std::set<std::string>(names.begin(), names.end()), juce::String(written.front().name));

    // Five header lines, then {"data": ...}, gzipped.
    juce::String text;
    if (const auto* header = field(doc, "header").getArray())
        for (const auto& line : *header) text << line.toString() << "\n";
    auto root = object();
    put(root, "data", data);
    text << juce::JSON::toString(root);
    const fs::path out = where / (name + ".xpj");
    const fs::path temporary = where / (name + ".xpj.part");
    {
        juce::FileOutputStream file(juce::File(temporary.string()));
        if (!file.openedOk()) return "Cannot write " + out.string();
        file.setPosition(0);
        file.truncate();
        juce::GZIPCompressorOutputStream packed(file, 6, juce::GZIPCompressorOutputStream::windowBitsGZIP);
        const auto utf8 = text.toUTF8();
        if (!packed.write(utf8.getAddress(), utf8.sizeInBytes() - 1)) return "Cannot write " + out.string();
        packed.flush();
    }
    fs::rename(temporary, out, error);
    if (error) return "Cannot write " + out.string();
    return {};
}

} // namespace melodee
