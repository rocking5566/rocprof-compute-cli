# RCV CLI — Design

Date: 2026-09-09
Status: Approved

## Purpose

Give an AI agent a way to analyze ATT (Advanced Thread Trace) captures without the Qt GUI.
The consumer is an agent working within a limited context window, not a human reading a
terminal report. Every design decision below follows from that: output is aggregated and
bounded by default, and the expensive work happens once so that follow-up queries are cheap.

The GUI is out of scope. Its sources stay in the tree but are not built by default, so the
fork can still be rebased onto upstream. The official build is used when a GUI is wanted.

## Scope

First version covers three analyses:

1. **Hotspot** — per-ASM-line and per-source-line cycle attribution (issue / stall / idle),
   with stall-reason and instruction-type breakdown.
2. **Hidden latency** — how much of each line's latency is masked by other waves, so that
   the agent is not led to optimize a stall that costs nothing.
3. **Occupancy** — wave concurrency over time, per SE.

Deferred: memory latency (`SQ_INST_LEVEL_*`), flamegraph / marker scopes, per-wave
instruction timeline dumps.

## Reference capture

`/tmp/local_read_v_artifacts/v2_att_newserver/captures/B32768_r2` — gfx1250, 16 shader
engines, one dispatch. Sizes that drive the design:

| Quantity | Value |
|---|---|
| ASM lines (`code.json`) | 2,521 |
| Waves | 128 |
| Instructions per wave | ~305,583 |
| Wave JSON total | 1.02 GB |
| Raw `.att` files | 16 × 7.9 MB |

Listing all 2,521 ASM lines with per-line metrics would cost roughly 150–250k tokens, which
exceeds the agent's budget. Aggregation and top-N truncation are therefore mandatory, not a
nicety.

## Architecture

### `rcv_core` static library

Links `Qt6::Core` only — no Widgets, no Gui. Members are the data-layer sources already
proven to build headlessly by `tests/att/CMakeLists.txt`:

- `src/data/*.cpp` including `src/data/waitcnt/*.cpp`
- `src/analysis/*.cpp`
- `src/code/codeload.cpp`
- `src/config/*.cpp`
- `src/wave/othersimd.cpp`
- `src/util/custom_layouts.cpp`, `src/util/jsonrequest.cpp`

Three changes are required to make this compile without Qt Widgets:

1. **Break `wavemanager.h` → `graphics/canvas.h`.** `WaveInstance` pulls in the whole canvas
   header solely for `std::vector<Canvas::WaitList> waitcnt`. Move the `WaitList` type into a
   new plain header `src/data/waitlist_types.h`; `graphics/canvas.h` includes it instead of
   defining it. Guard `TokenGroup::Draw(QPainter&)` with `#ifdef RCV_BUILD_GUI`.

2. **Extract `TraceLoader`** into `src/data/trace_loader.{h,cpp}`. Move the load
   orchestration out of `MainWindow::LoadInputImpl` (`src/mainwindow.cpp:1011`):
   `detectInput()` → dispatch on `InputType` → JSON path or `TraceDecoderEmitter` → populate
   `DataStore`. Drop all `QMessageBox`, progress-bar, and widget updates; report errors
   through a return value. The existing `LoadInputForTests` hook shows this path already runs
   headlessly.

3. **Add `rcv-cli`** under `src/cli/`, linking `rcv_core`.

### Build options

| Option | Default | Notes |
|---|---|---|
| `RCV_BUILD_GUI` | `OFF` | GUI sources remain in tree, unbuilt, to preserve upstream rebase |
| `RCV_BUILD_CLI` | `ON` | |
| `TRACE_DECODER_ROOT` | unset | Set to `~/rocm7.15.0a20260712` to use the installed decoder; avoids the git fetch path in `cmake/FetchTraceDecoder.cmake` |

### Where the numbers come from

The CLI reads aggregates directly from the data layer and never touches a widget:

- **Hotspot** — `DataStore::code[i].line` (a `CodeData::Line` in `src/code/codeload.hpp`,
  a plain struct) supplies `latency_sum`, `stall_sum`, `idle_sum`, `hitcount`,
  `stallreasons`, `type`, `inst`, `cppline`, `addr`, `codeobj_id`. On the JSON path these are
  filled by `CodeData::LoadCode()` reading `code.json`; on the ATT path by
  `trace_decoder_emitter.cpp`. Both are non-GUI.
- **Hidden latency** — call `HiddenLatencyAnalysis::analyze(store)` and read
  `store.hidden_latency_by_line` (a `std::map<int, HiddenLatency>`). Do **not** call
  `applyToAsm()`; that exists only to push values into `ASMCodeline` widgets.
- **Occupancy** — `store.occupancy_by_se`, `store.wave_hierarchy`, `store.wave_state_series`.

### Metric definitions

Stated explicitly because the sort keys depend on them. These follow `Latency` in
`src/code/hotspot.hpp`:

```
issue    = latency - stall          // latency is active time; stall is its stalled part
total    = latency + idle           // wall time attributable to the line
hidden   = hidden.idle + hidden.stall + hidden.issue
exposed  = total - hidden
```

Each `hidden.*` component is clamped to its corresponding non-hidden component, so
`0 <= hidden <= total` and `exposed >= 0` hold by construction.

## Command interface

Two phases: analyze once, query many. This exists because hidden-latency analysis must parse
all 128 wave JSON files (1 GB) — tens of seconds to minutes. Paying that on every drill-down
query would make the drill-down model unusable.

### Phase 1 — `rcv-cli analyze <trace_path> [-o digest.json]`

Runs the full load plus hidden-latency analysis and writes a **digest**. Input is either a
`ui_output_*` directory or a directory of `.att` files; the format is auto-detected via
`detectInput()`.

Digest contents (no per-instruction timestamps, so it stays around 500 KB):

- `meta` — gfxip, gfxv, SE/CU/SIMD counts, wave count, trace begin/end, kernel name
- `lines[]` — one entry per ASM line: `index`, `addr`, `codeobj_id`, `inst`, `cppline`,
  `type`, `hitcount`, `latency`, `stall`, `idle`, `hidden{idle,stall,issue}`,
  `stallreasons[16]`
- `occupancy` — binned time series (200 bins by default) plus per-SE statistics
- `waves[]` — per wave: se, cu, simd, slot, begin, end (index only, no instructions)

All 2,521 lines go into the digest rather than only the top-N, because the digest is consumed
by the CLI, not by the agent. Queries filter it. This means changing sort key or drilling into
a different region never requires re-reading the 1 GB source.

### Phase 2 — query commands

All read the digest and complete in milliseconds.

| Command | Purpose |
|---|---|
| `summary` | Global overview: total cycles, issue/stall/idle split, stall-reason ranking, instruction-type distribution, occupancy summary, top-10 hot source lines. The agent's entry point; fixed small size. |
| `hotspot [--top N] [--by source\|asm] [--sort exposed\|total\|stall\|idle]` | Hotspot ranking. Defaults: `--by source --sort exposed --top 20`. |
| `asm --range A-B` \| `--around <index> [--context N]` | Per-instruction detail for a region, including per-line stall reasons. |
| `occupancy [--bins N] [--se N]` | Wave concurrency over time. |

**The default sort key is `exposed` (`total - hidden`), not `total`.** Latency masked by other
waves costs nothing to fix; sorting by `total` would steer the agent toward false hotspots.
`--sort total` remains available.

### Output format

Aligned text tables by default; `--json` for machine consumption. For the same 50 rows, JSON
costs roughly 3× the tokens of an aligned table, and the agent's context is the binding
constraint. JSON stays available for programmatic post-processing.

## Testing

### Layer 1 — cross-check against rocprofv3's CSV

`stats_ui_output_agent_29410_dispatch_19.csv` independently reports
`Hitcount, Latency, Stall, Idle, Source` for all 2,521 lines. Compare the digest line-by-line
against it and require **exact equality** (integers, no tolerance). This catches any loading,
index-alignment, or aggregation error.

Spot-checked during design: CSV row 2 reads `128,128,0,2521539` and the matching `code.json`
entry ends `...,128,128,0,2521539` — confirming hotspot values originate from `code.json`.

Gated on `RCV_CLI_TEST_TRACE` pointing at a capture; skipped when unset, so the 1.2 GB
capture is not committed.

### Layer 2 — hidden-latency invariants

No external ground truth exists for this algorithm, so assert properties instead. For every
line: `0 <= hidden.idle <= idle`, `0 <= hidden.stall <= stall`,
`0 <= hidden.issue <= latency - stall`, and `exposed >= 0`, using the metric definitions
above. Add a small synthetic fixture of two hand-constructed
overlapping waves, with hand-computed expected values, committed as a golden test.

### Layer 3 — query-layer unit tests

The digest is plain data, so `summary` / `hotspot` / `asm` / `occupancy` filtering, sorting,
and binning are tested against a small hand-written digest with no real trace. Covers sort
keys (especially `exposed`), `--top N` truncation, and bin boundaries.

### Layer 4 — smoke test

Run `analyze` → `summary` → `hotspot` → `asm --around <top-1>` against a real trace; assert
exit code 0 and non-empty output. Record analyze wall time and peak RSS, since the 1 GB input
is where this tool is most likely to fail.

## Development order

JSON path (`ui_output_*`) first, all the way through Layer 1 validation. Then the `.att`
decoder path. The CSV ground truth only exists on the JSON path, so the numbers get pinned
first and the decoder path is then made to match them.

## Risks

- **Memory.** 128 waves × ~305k tokens is on the order of 1–2 GB resident if all waves stay
  cached during hidden-latency analysis. `WaveInstance` already has a cache with
  `InvalidadeCache()`; if peak RSS proves unacceptable, evict waves after each
  `(se, simd)` scope completes, since `analyzeScoped` works one scope at a time.
- **Extraction regressions.** Moving `LoadInputImpl` out of `MainWindow` risks dropping a
  step that only the GUI path performed. Layer 1's exact-equality check against the CSV is
  the guard: if a load step is missing, the totals will not match.
