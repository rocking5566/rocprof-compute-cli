// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#pragma once

#include "data/trace_loader.h"
#include "json/include/nlohmann/json.hpp"

class DataStore;

namespace rcv
{
// Zero-based occurrences of the selected static line in chronological order.
// Bounds are inclusive; these are not inferred source-loop iteration IDs.
using IterationRange = std::optional<std::pair<int, int>>;
// Requires a complete selected wave. No hidden analysis or other wave loads.
nlohmann::json queryWait(
    DataStore& store,
    const WaveSelection& selected,
    int line,
    int top,
    int context,
    IterationRange iterations = std::nullopt
);
std::string renderWait(const nlohmann::json& result);
// Expects deferred loading. Missing SE/SIMD/slot/instance filters are wildcards.
// CU filtering is intentionally unsupported: JSON manifests may omit CU IDs.
nlohmann::json queryWaitSummary(
    DataStore& store, const WaveSelection& filters, int line, int max_waves, IterationRange iterations = std::nullopt
);
std::string renderWaitSummary(const nlohmann::json& result);
} // namespace rcv
