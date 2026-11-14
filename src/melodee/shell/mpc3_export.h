// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <string>

// A Melodee project as an MPC 3 project (MPC One, Live, Key, X and the MPC 3
// desktop app): "<name>.xpj" (gzip around five header lines and a JSON
// document) and "<name>_[ProjectData]" beside it, holding the WAV files. The
// document is a copy of a project MPC 3 saved (mpc3_template.json, made by
// tools/mpc3_template.py) with Melodee's tracks, pads and patterns put in.
//
// Kit tracks are written: each becomes a Drum track, each pad a pad of WAV
// layers, each pattern number a sequence, the tempo and time signature with
// them. A pad of an instrument of key zones (an SFZ, a keygroup program), or a
// WAV pad played at other pitches, becomes a Keygroup track of its own. Other
// tracks, and sounds that are not WAV files, are counted in the report and left out.
namespace melodee {

struct Mpc3Export {
    unsigned tracks = 0, keygroups = 0, sequences = 0, pads = 0, notes = 0, samples = 0; // tracks: Drum tracks; keygroups: Keygroup tracks
    unsigned skipped_tracks = 0; // tracks that are not kits: synths, plug-ins, MIDI, audio
    unsigned skipped_pads = 0;   // pads, layers or zones not written: not a WAV file (an SF2's, a FLAC), past eight layers
    unsigned missing = 0;        // WAV files not where the project says
};

// Writes <folder>/<name>.xpj and <folder>/<name>_[ProjectData]/. Empty on success. Reads and copies files:
// never on the MIDI or audio thread.
std::string export_mpc3_project(const Project& project, const PatternBank& patterns, const std::string& folder,
                                const std::string& name, Mpc3Export& report);

} // namespace melodee
