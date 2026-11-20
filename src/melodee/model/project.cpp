// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/project.h"

namespace melodee {

// A Project's construction is optimized here once (project.h, Project()).
Project::Project() = default;

void PatternBank::clear() {
    for (auto& track : at)
        for (auto& pattern : track) pattern = Pattern {};
}

} // namespace melodee
