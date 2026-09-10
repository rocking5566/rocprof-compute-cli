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

#include "data/trace_loader.h"

#include <filesystem>
#include "code/codeload.hpp"
#include "data/datastore.h"
#include "data/input_detector.h"
#include "data/json_emitter.h"
#include "data/record_dispatcher.h"
#include "data/record_handlers.h"
#include "data/shaderdata.h"
#include "data/wavemanager.h"
#ifdef RCV_HAS_TRACE_DECODER
#    include "data/trace_decoder_emitter.h"
#endif

namespace fs = std::filesystem;

namespace rcv
{

LoadResult loadTrace(const std::string& input_path, DataStore& store, ForceFormat force)
{
    LoadResult result;

    std::error_code exists_error;
    if (!fs::exists(input_path, exists_error))
    {
        result.error = "input path does not exist: " + input_path;
        return result;
    }

    InputInfo info = detectInput(input_path);
    if (force == ForceFormat::JsonDir)
        info.type = InputType::JSON_DIR;
    else if (force == ForceFormat::AttFiles)
        info.type = InputType::ATT_FILES;
    if (info.type == InputType::UNKNOWN)
    {
        result.error = "could not determine input format for: " + input_path;
        return result;
    }

    // Both caches are keyed by generated wave filenames and the code.json path,
    // which repeat across traces. Clear them before any emitter resolves
    // markers or lazily loads a wave (mirrors mainwindow.cpp:1115-1116).
    WaveInstance::main_wave.reset();
    WaveInstance::InvalidadeCache();

    store.clear();
    store.ui_dir = input_path;
    if (!store.ui_dir.empty() && store.ui_dir.back() != '/' && store.ui_dir.back() != '\\') store.ui_dir.push_back('/');

    // The dispatcher fans each decoded record out to the handlers that own the
    // corresponding DataStore field. All seven are required: dropping one
    // leaves that field silently empty (mainwindow.cpp:1124-1139).
    RecordDispatcher dispatcher;
    WaveHandler wave_handler(store);
    OccupancyHandler occ_handler(store);
    CounterHandler ctr_handler(store);
    ShaderDataHandler shaderdata_handler(store);
    OtherSimdHandler other_simd_handler(store);
    RealtimeHandler rt_handler(store);
    MetadataHandler meta_handler(store);
    dispatcher.addHandler(&wave_handler);
    dispatcher.addHandler(&occ_handler);
    dispatcher.addHandler(&ctr_handler);
    dispatcher.addHandler(&shaderdata_handler);
    dispatcher.addHandler(&other_simd_handler);
    dispatcher.addHandler(&rt_handler);
    dispatcher.addHandler(&meta_handler);

    try
    {
        switch (info.type)
        {
            case InputType::JSON_DIR:
            {
                // The GUI passes a callback that consults AppConfig and mirrors
                // the answer into a checkbox. The CLI always loads wave states:
                // hidden-latency analysis needs them, and there is no UI to toggle.
                JsonRecordEmitter emitter(store.ui_dir, dispatcher, store, [](const DataStore&) { return true; }, true);
                emitter.run();
                break;
            }
            case InputType::ATT_FILES:
            {
#ifdef RCV_HAS_TRACE_DECODER
                if (info.att_file_info.size() != info.att_files.size())
                {
                    info.att_file_info.clear();
                    for (const auto& p : info.att_files) info.att_file_info.push_back(parseAttFilename(p));
                }

                TraceDecoderEmitter emitter(info, dispatcher, store);
                emitter.run();
                if (!emitter.parseErrors().empty())
                {
                    result.error = "ATT decode failed: " + emitter.parseErrors().front();
                    return result;
                }
                if (store.code.empty())
                {
                    result.error = "decoder produced no code; check that the .out code-object files "
                                   "sit alongside the .att files";
                    return result;
                }
#else
                result.error = "this build has no trace decoder; rebuild with TRACE_DECODER_ROOT";
                return result;
#endif
                break;
            }
            default:
                result.error = "unsupported input format for the CLI (only ui_output "
                               "directories and .att files are supported)";
                return result;
        }

        // Load once into the shared wave cache, so later analysis cannot mistake
        // a manifest count for successfully loaded data. Empty instructions are valid.
        size_t waves = 0;
        store.forEachWave([&](const DataStore::WaveCoordinate&, const WaveEntry& entry)
        {
            auto wave = store.getWave(entry);
            if (!wave || !wave->load_complete)
                throw std::runtime_error("could not load complete wave: " + entry.id);
            ++waves;
        });
        if (waves == 0)
        {
            result.error = "input contains no waves; empty or unsupported thread-trace capture";
            return result;
        }
    }
    catch (const std::exception& e)
    {
        result.error = std::string("load failed: ") + e.what();
        return result;
    }

    if (store.code.empty())
    {
        result.error = "load produced no ASM lines; is this a thread-trace capture?";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace rcv
