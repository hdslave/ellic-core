// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/xpm_write.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace melodee {
namespace fs = std::filesystem;

namespace {
std::string escaped(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}
std::string number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6f", value);
    return text;
}
std::string tag(int indent, const char* name, const std::string& value) {
    return std::string(size_t(indent), ' ') + "<" + name + ">" + value + "</" + name + ">\n";
}
std::string tag(int indent, const char* name, int value) { return tag(indent, name, std::to_string(value)); }
const char* truth(bool value) { return value ? "True" : "False"; }

// An unused layer as the MPC writes one: the four slots are always there.
std::string layer_text(unsigned number_, const XpmLayer* l) {
    std::string s = "            <Layer number=\"" + std::to_string(number_) + "\">\n";
    const int i = 14;
    s += tag(i, "Active", truth(true));
    s += tag(i, "Volume", number(1.0));
    s += tag(i, "Pan", number(0.5));
    s += tag(i, "Pitch", number(0.0));
    s += tag(i, "TuneCoarse", 0);
    s += tag(i, "TuneFine", 0);
    s += tag(i, "VelStart", l ? l->low_velocity : 0);
    s += tag(i, "VelEnd", l ? l->high_velocity : 127);
    s += tag(i, "SampleStart", 0);
    s += tag(i, "SampleEnd", l ? int(l->frames) : 0);
    s += tag(i, "Loop", truth(false));
    s += tag(i, "LoopStart", 0);
    s += tag(i, "LoopEnd", l ? int(l->frames) : 0);
    s += tag(i, "LoopTune", 0);
    s += tag(i, "Mute", truth(false));
    s += tag(i, "RootNote", l ? l->root + 1 : 0); // one past the MIDI note, as the MPC writes it
    s += tag(i, "KeyTrack", truth(true));
    s += tag(i, "SampleName", l ? escaped(l->sample) : std::string());
    s += tag(i, "SampleFile", "");
    s += tag(i, "SliceIndex", 128);
    s += tag(i, "Direction", 0);
    s += tag(i, "Offset", 0);
    s += tag(i, "SliceStart", 0);
    s += tag(i, "SliceEnd", l ? int(l->frames) : 0);
    s += tag(i, "SliceLoopStart", 0);
    s += tag(i, "SliceLoop", 0);
    s += "            </Layer>\n";
    return s;
}
} // namespace

double xpm_envelope_value(double seconds) {
    // keygroup_seconds(): 0.001 * 100000^value
    if (seconds <= 0.001) return 0;
    return std::clamp(std::log10(seconds * 1000.0) / 5.0, 0.0, 1.0);
}

std::string xpm_text(const XpmProgram& program) {
    std::string s;
    s += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n";
    s += "<MPCVObject>\n";
    s += "  <Version>\n";
    s += tag(4, "File_Version", "2.1");
    s += tag(4, "Application", "MPC-V");
    s += tag(4, "Application_Version", "2.10.1");
    s += tag(4, "Platform", "Linux");
    s += "  </Version>\n";
    s += "  <Program type=\"Keygroup\">\n";
    const int p = 4;
    s += tag(p, "ProgramName", escaped(program.name));
    s += tag(p, "Volume", number(0.707946)); // 0 dB on the MPC's curve
    s += tag(p, "Pan", number(0.5));
    s += tag(p, "Pitch", number(0.0));
    s += tag(p, "TuneCoarse", 0);
    s += tag(p, "TuneFine", 0);
    s += tag(p, "Mono", truth(false));
    s += tag(p, "Program_Polyphony", 32);
    s += tag(p, "PortamentoTime", number(0.0));
    s += tag(p, "PortamentoLegato", truth(false));
    s += tag(p, "PortamentoQuantized", truth(false));
    s += tag(p, "KeygroupMasterTranspose", number(0.5));
    s += tag(p, "KeygroupNumKeygroups", int(std::min<size_t>(program.keygroups.size(), 128)));
    s += tag(p, "KeygroupPitchBendRange", number(0.0));
    s += tag(p, "KeygroupWheelToLfo", number(0.0));
    s += tag(p, "KeygroupAftertouchToFilter", number(0.0));
    s += "    <Instruments>\n";
    const double release = xpm_envelope_value(program.release);
    unsigned n = 0;
    for (const auto& k : program.keygroups) {
        if (n == 128) break;
        s += "      <Instrument number=\"" + std::to_string(++n) + "\">\n";
        const int i = 8;
        s += tag(i, "Volume", number(0.707946));
        s += tag(i, "Pan", number(0.5));
        s += tag(i, "Pitch", number(0.0));
        s += tag(i, "TuneCoarse", 0);
        s += tag(i, "TuneFine", 0);
        s += tag(i, "Mono", truth(false));
        s += tag(i, "Polyphony", 1);
        s += tag(i, "FilterKeytrack", number(0.0));
        s += tag(i, "LowNote", k.low);
        s += tag(i, "HighNote", k.high);
        s += tag(i, "IgnoreBaseNote", truth(false));
        s += tag(i, "ZonePlay", 1); // the layers its velocity reaches
        s += tag(i, "MuteGroup", 0);
        s += tag(i, "OneShot", truth(false));
        s += tag(i, "TriggerMode", 2); // held while the key is
        s += tag(i, "FilterType", 0); // off: the plug-in's sound as it was printed
        s += tag(i, "Cutoff", number(1.0));
        s += tag(i, "Resonance", number(0.0));
        s += tag(i, "FilterEnvAmt", number(0.0));
        s += tag(i, "VelocityToPitch", number(0.0));
        s += tag(i, "VelocityToFilter", number(0.0));
        s += tag(i, "VelocityToStart", number(0.0));
        s += tag(i, "VelocitySensitivity", number(k.layers.size() > 1 ? 0.0 : 1.0)); // layered: the layers hold the dynamics
        s += tag(i, "VolumeHold", number(0.0));
        s += tag(i, "VolumeAttack", number(0.0));
        s += tag(i, "VolumeDecay", number(0.0));
        s += tag(i, "VolumeSustain", number(1.0));
        s += tag(i, "VolumeRelease", number(release));
        s += tag(i, "VolumeDecayType", truth(true));
        s += "        <Layers>\n";
        for (unsigned l = 0; l < 4; ++l) s += layer_text(l + 1, l < k.layers.size() ? &k.layers[l] : nullptr);
        s += "        </Layers>\n";
        s += "      </Instrument>\n";
    }
    s += "    </Instruments>\n";
    s += "  </Program>\n";
    s += "</MPCVObject>\n";
    return s;
}

std::string write_xpm(const std::string& path, const XpmProgram& program) {
    const std::string text = xpm_text(program);
    const std::string part = path + ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return "cannot write " + fs::path(path).filename().string();
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) return "cannot write " + fs::path(path).filename().string();
    }
    std::error_code e;
    fs::rename(part, path, e);
    if (e) { fs::remove(part, e); return "cannot write " + fs::path(path).filename().string(); }
    return {};
}

} // namespace melodee
