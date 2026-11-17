// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once

// Chop's pads hold sixteen slices at a time, as Push's grid holds them all: Pad Bank steps through them (A slices
// 1-16, B 17-32 ...), and the bank in view is the selected slice's. The bank of slice n, how many banks `count`
// slices fill (one at least), and the slice under `pad` (0-15) while slice `selected` is in hand.
namespace melodee {

inline unsigned slice_bank(unsigned slice) { return slice / 16; }
inline unsigned slice_banks(unsigned count) { return count > 16 ? (count + 15) / 16 : 1; }
inline unsigned pad_slice(unsigned selected, unsigned pad) { return slice_bank(selected) * 16 + pad % 16; }

} // namespace melodee
