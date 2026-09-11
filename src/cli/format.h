// MIT License
//
// Copyright (c) 2024-2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <string>
#include <vector>
#include "cli/digest.h"
#include "cli/queries.h"

namespace rcv
{
std::string renderTable(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows);
std::string formatPercent(int64_t part, int64_t whole);
std::string renderSummary(const Digest& d);
nlohmann::json summaryJson(const Digest& d);

int dominantStallReason(const LineDigest& l);

std::string renderHotspot(const Digest& d, const std::vector<HotspotRow>& rows);
nlohmann::json hotspotJson(const std::vector<HotspotRow>& rows);

std::string renderAsm(const Digest& d, const std::vector<LineDigest>& lines);
nlohmann::json asmJson(const Digest& d, const std::vector<LineDigest>& lines);

std::string renderOccupancy(const Digest& d, int se);
nlohmann::json occupancyJson(const Digest& d, int se);
std::string renderCoverage(const Digest& d, int top);
nlohmann::json coverageJson(const Digest& d, int top);
nlohmann::json groupedOccupancyJson(const Digest& d, bool by_simd, int se, int cu, int simd, int top);
std::string renderGroupedOccupancy(const nlohmann::json& result);
} // namespace rcv
