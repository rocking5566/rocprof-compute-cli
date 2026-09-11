// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#pragma once
#include "cli/digest.h"

namespace rcv
{
nlohmann::json compareDigests(const Digest& before, const Digest& after, int top);
std::string renderCompare(const nlohmann::json& result);
} // namespace rcv
