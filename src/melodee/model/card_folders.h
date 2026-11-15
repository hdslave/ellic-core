// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <filesystem>

namespace melodee {

// Melodee's own folders on the card that hold nothing to open on the MPC: Melodee/Cache (samples decoded at the
// device's rate) and Melodee/Crashes (crash reports). The MPC's browsers (plug-ins, samples, projects) leave them out;
// the web page's file list still shows them, so crash reports can be downloaded.
inline bool melodee_own_folder(const std::filesystem::path& dir) {
    const auto name = dir.filename().string();
    return dir.parent_path().filename() == "Melodee" && (name == "Cache" || name == "Crashes");
}

} // namespace melodee
