// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#pragma once

#include "data/trace_loader.h"
#include "json/include/nlohmann/json.hpp"

class DataStore;

namespace rcv
{
// Requires a complete selected wave. No hidden analysis or other wave loads.
nlohmann::json queryWait(DataStore& store, const WaveSelection& selected, int line, int top, int context);
std::string renderWait(const nlohmann::json& result);
} // namespace rcv
