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

**Links no Qt at all.** An earlier revision of this spec called for `Qt6::Core` + `Qt6::Gui`,
on the grounds that `config.cpp` needs `QPalette`/`QColor`. That turned out to be avoidable: the
CLI wants only `StyleColor::name`, and the token and stall-reason tables behind it are plain
JSON. Every colour-returning API is now compiled only when `RCV_BUILD_GUI` is set, and
`StreamRequest`'s network half is too — `ReadFromFile` was already `std::ifstream`. A CLI-only
build therefore skips `find_package(Qt6)` and the AUTOMOC/AUTOUIC/AUTORCC generators entirely
and runs on a machine with no Qt installed.

Three translation units left `rcv_core` because nothing in it reaches them:
`config/appconfig.cpp` (GUI settings persistence), `data/marker_colors.cpp`, and
`analysis/annotation.cpp`.

Members (see `cmake/RcvCore.cmake` for the authoritative list):

- `src/data/*.cpp` including `src/data/waitcnt/*.cpp`, less `marker_colors.cpp`
- `src/analysis/*.cpp`, less `annotation.cpp`
- `src/code/codeload.cpp`
- `src/config/config.cpp` (not `appconfig.cpp`)
- `src/wave/othersimd.cpp`, `src/wave/token.cpp`
- `src/util/jsonrequest.cpp`, `src/util/memtracker.cpp`

Note `src/util/custom_layouts.cpp` is **not** a member: it is Qt Widgets layout code. It was
listed here in an earlier revision, inherited from `tests/att/CMakeLists.txt`, which links
`Qt6::Widgets` precisely because of it.

The four changes below were the ones identified up front. Executing them surfaced eight more,
each visible only once the previous was fixed — `data/waitcnt/analysis.h` depending on
`Canvas::WaitList`, a dead `<QPushButton>` in `wave/token.h`, `MemTracker::count` being defined
inside `custom_layouts.cpp`, and so on. Commit `e69ebd0` lists them all. Treat this list as a
starting point rather than a complete inventory:

1. **Break `wavemanager.h` → `graphics/canvas.h`.** `WaveInstance` pulls in the whole canvas
   header solely for `std::vector<Canvas::WaitList> waitcnt` (`wavemanager.h:31`,
   `canvas.h:48`). Move the `WaitList` type into a new plain header
   `src/data/waitlist_types.h`; `graphics/canvas.h` includes it instead of defining it.
   Guard `TokenGroup::Draw(QPainter&)` (`wavemanager.cpp:173-226`) with
   `#ifdef RCV_BUILD_GUI`. The guard must also cover `wavemanager.cpp`'s
   `#include <QPainter>` (line 24) and `#include "mainwindow.h"` (line 29) — the latter
   exists only for the `MainWindow::getScaling()` call at line 216, which is inside `Draw()`.

2. **Move `applyToAsm()` out of `hidden_latency.cpp`.** That file includes `code/asmcode.h`
   (line 32), which pulls in `QLabel`/`QPushButton`/`QWidget` (`asmcode.h:23-25`), so
   `src/analysis` does not currently compile without Qt Widgets. Move `applyToAsm()` and
   `clearAsmHidden()` into a new GUI-only TU (`src/code/hidden_latency_asm.cpp`, built only
   when `RCV_BUILD_GUI`), leaving a no-op stub for CLI builds; `hidden_latency.cpp` then drops
   the `asmcode.h` include. Note that `analyze()` calls `applyToAsm()` itself
   (`hidden_latency.cpp:347`), so the CLI cannot simply decline to call it — the call must
   become a no-op by construction.

3. **Extract `TraceLoader`** into `src/data/trace_loader.{h,cpp}`. Move the load
   orchestration out of `MainWindow::LoadInputImpl` (`src/mainwindow.cpp:1011`):
   `detectInput()` → dispatch on `InputType` → JSON path or `TraceDecoderEmitter` → populate
   `DataStore`. The emitter orchestration (lines 1138-1210) is already widget-free and is the
   extractable core. Drop all `QMessageBox`, progress-bar, and widget updates; report errors
   through a return value. The existing `LoadInputForTests` hook shows this path already runs
   headlessly.

   These post-load calls are **view setup, not load logic** — they stay in `MainWindow` after
   `TraceLoader` returns, and must not be pulled into `rcv_core`:
   `utilization_content->SetOtherSimdRecords` (1275), marker flamegraph construction
   (1288-1310), statusBar diagnostics (1315-1340), and `CreateWavesPlot` /
   `CreateOccupancyPlot` / `CreateGlobalView` (1360-1375).

4. **Add `rcv-cli`** under `src/cli/`, linking `rcv_core`.

### The wave-load budget must not be inherited

`MainWindow` refuses to load all waves when their total size exceeds
`WAVE_LOAD_BUDGET_BYTES = 200 MB` (`mainwindow.cpp:125`). Headless, `allowFullWaveLoad`
returns `false` without prompting (`mainwindow.cpp:782`), and hidden-latency analysis only
runs when that flag is true (`mainwindow.cpp:1361`).

The reference capture's wave JSONs total **1.02 GB**, five times the budget. A faithful
extraction of `LoadInputImpl` would therefore **silently produce no hidden latency at all** on
exactly the capture this tool is being built for — no error, just missing data.

`TraceLoader` must not carry the budget gate. The CLI always loads all waves, and `analyze`
fails loudly rather than degrading quietly if that is impossible.

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

Each `hidden.*` component is bounded by its corresponding non-hidden component because
`compute_interval` (`hidden_latency.cpp:105-116`) intersects the utilization set with the
token's own idle/stall/issue intervals, so `0 <= hidden <= total` and `exposed >= 0` hold.

**`include_idle` is pinned to `true`.** The GUI carries this as a parameter throughout
(`Latency::total`, `hiddenTotal`, `nonHidden` in `hotspot.hpp:31-44`) driven by the global
`HorizontalHotspot::show_idle_time`, which defaults to `true` (`hotspot.cpp:117`). The CLI has
no view state and nothing toggles it, so the flat formulas above are exact. If an
`--active-only` mode is ever added, that is where `include_idle=false` and the adjusted
formulas belong. Related defaults: `is_pcs_enabled = false` (`hotspot.cpp:115`), so the `pcs`
half of `HorizontalHotspot` is inert for SQTT captures.

### Source attribution is one-to-many, with replicated cost

`ASMLine` splits `cppline` on `" -> "` and calls `add_latency()` with the **full**
`latency_sum` / `stall_sum` / `idle_sum` for *every* frame in the chain
(`asmcode.cpp:206-227`). This is inclusive-cost semantics: each inlined frame is charged the
whole cost. The cost is **replicated, not divided** — an implementation that splits the
latency among frames would disagree with the GUI. Source-line totals therefore do not sum to
the kernel total when inlining is present.

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
| `summary` | Global overview: total cycles, issue/stall/idle split, stall-reason ranking, instruction-type distribution, occupancy summary, top-10 hot ASM lines, and source-attribution coverage. The agent's entry point; fixed small size. |
| `hotspot [--top N] [--by asm\|source] [--sort exposed\|total\|stall\|idle]` | Hotspot ranking. Defaults: `--by asm --sort exposed --top 20`. |
| `asm --range A-B` \| `--around <index> [--context N]` | Per-instruction detail for a region, including per-line stall reasons. |
| `occupancy [--bins N] [--se N]` | Wave concurrency over time. |

**The default sort key is `exposed` (`total - hidden`), not `total`.** Latency masked by other
waves costs nothing to fix; sorting by `total` would steer the agent toward false hotspots.
`--sort total` remains available.

**`--by asm` is the default, not `--by source`.** In the reference capture only **1 of 2,521**
ASM lines carries a non-empty `cppline`, and that one is the kernel-name comment rather than a
source location; there are no source snapshots in the `ui_output` directory and no `" -> "`
chain anywhere. The kernel was built without `-g`. Defaulting to source view would hand the
agent an empty table. `summary` therefore reports source-attribution coverage explicitly —
e.g. `source attribution: 1/2521 lines (0.04%) — kernel likely built without -g` — so an empty
source view is explained rather than silently blank.

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
exit code 0 and non-empty output. Record analyze wall time and peak RSS.

Additionally assert that **total hidden latency across all lines is non-zero**. Exit code 0
with an empty `hidden` column is precisely the failure mode the wave-load budget produces, and
it would otherwise pass every other check in this plan.

## Development order

JSON path (`ui_output_*`) first, all the way through Layer 1 validation. Then the `.att`
decoder path. The CSV ground truth only exists on the JSON path, so the numbers get pinned
first and the decoder path is then made to match them.

## Risks

- **Silent loss of hidden latency.** The highest-severity risk, addressed above: inheriting
  the 200 MB wave-load gate would drop analysis 4 of the 3 in scope without any error. The
  smoke test must assert that hidden latency is non-zero on the reference capture, not merely
  that the command exits 0.
- **Extraction regressions.** Moving `LoadInputImpl` out of `MainWindow` risks dropping a
  step that only the GUI path performed. Layer 1's exact-equality check against the CSV is
  the guard: if a load step is missing, the totals will not match.
- **Memory — minor.** `WaveInstance::Get` caches into an unbounded `reader_cache`
  (`wavemanager.cpp:36,53`) that is only cleared wholesale by `InvalidadeCache()`, so all 128
  waves stay resident through `analyze()`. That is ~940 MB of `Token` (24 bytes each,
  `wave/token.h`) plus per-wave `line_to_clock` and `exec` vectors, so roughly 2-4 GB. The
  target machine has 2,267 GB of RAM, so this is not a constraint. If it ever becomes one,
  `analyzeScoped` works one `(se, simd)` scope at a time and offers a natural eviction point.
