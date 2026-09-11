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

#include <optional>
#include <string>
#include <vector>

class DataStore;

namespace rcv
{

struct LoadResult
{
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
};

enum class ForceFormat
{
    Auto,
    JsonDir,
    AttFiles
};

struct WaveSelection
{
    int se = -1, simd = -1, slot = -1, instance = -1;
    int cu = -1; // optional check, not a substitute for the wave instance
};

enum class WaveLoadMode
{
    Complete,
    Deferred
};

/// Headless trace load. Detects the input format from `input_path`, populates
/// `store`, and by default loads every wave — unlike MainWindow, which gates full
/// wave loading behind a 200 MB budget and would silently skip hidden-latency
/// analysis on large captures.
/// A selection materializes only that wave and skips JSON marker resolution.
/// ATT decoding stays capture-wide to preserve ASM line identities.
/// Deferred mode loads the manifest/listing (and decodes ATT once), leaving
/// wave completeness validation to the bounded query. Not for analyze.
LoadResult loadTrace(
    const std::string& input_path,
    DataStore& store,
    ForceFormat force = ForceFormat::Auto,
    std::optional<WaveSelection> selected = std::nullopt,
    WaveLoadMode wave_mode = WaveLoadMode::Complete
);

} // namespace rcv
