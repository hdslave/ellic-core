// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/chop_slices.h"
#include <cstdint>
#include <string>

// A sample's slices kept in its WAV file, as the MPC 3 (and the stock MPC Sample) keep them: the "atem" chunk, JSON,
// "value0" holding "defaultSlice", "Num slices" and "Slice 0", "Slice 1" ... each with its own Start and End (frames
// of the file; End past the slice's last frame), and the "slic" chunk beside it, the same slices in binary
// (u16 1, u16 count, u16 0, then each slice's u32 Start, u32 End, u32 LoopStart and u16 LoopMode).
namespace melodee {

// The file's slices as a chop (Manual) in the model's frames: its region the file's default slice (to 0: the end),
// each slice its start and end (0: the end). Unused when the file has none or is not a WAV.
SampleChop read_mpc_slices(const std::string& path);

// The chop's slices written into the WAV file at `path` (its frames at the file's rate): the "atem" chunk's slices
// replaced (its other values kept; one made when there is none) and the "slic" chunk written again; a chop with
// no slices takes them out. The file is written beside and renamed over. False when it is not a WAV or cannot be
// written (the file is then as it was).
bool write_mpc_slices(const std::string& path, const SampleChop& chop);

} // namespace melodee
