# ATT Diagnostic Drill-down Implementation Plan

> **For agentic workers:** Use test-first implementation and task review. The
> existing impl-cli Herdr session has ended; the coordinator executes locally in
> the user-selected checkout and build directory. Do not create a new worktree.

**Goal:** Implement the three user-approved CLI additions with bounded output.

**Architecture:** Add digest-backed aggregate queries and an on-demand selected
wave wait query using the existing loaders and wait-counter dependency analysis.

**Tech Stack:** C++20, nlohmann JSON, CMake, GoogleTest, Qt-free rcv_core.

**Spec:** `docs/superpowers/specs/2026-09-10-rcv-cli-drilldown-design.md`.

## Global constraints

- Existing analyze remains strict and atomic. Old version-1 digests remain readable.
- Hidden values remain raw with per-component read-time clamps; include_idle=true.
- Opcode/coverage/occupancy top defaults to 20 and caps at 1000.
- Wait top defaults to 3 and caps at 20; context defaults to 3 and caps at 10;
  dependency output caps at 32 with truncation metadata.
- Numeric errors exit 2 before input access; runtime failures exit 1.
- Build in `build`, ROCm prefix is `~/rocm7.15.0a20260712`, LD_LIBRARY_PATH unset.
- Preserve the reference capture, historical artifacts, shared GUI behavior and
  exact CSV/ATT direct-field tests. No publication.

## Task 1: Digest aggregates, opcode grouping and coverage

Files: `src/cli/digest.{h,cpp}`, `digest_builder.cpp`, `queries.{h,cpp}`,
`format.{h,cpp}`, `main.cpp`; tests in `tests/cli`.

- [x] Add behavioral regressions before implementation: two differently addressed
  `s_wait_dscnt` lines become one opcode row with summed hits/costs, comments are
  omitted, and an old digest's two SE/CU/SIMD groups yield the correct coverage.
- [x] Run the new regression cases against the existing binary and record RED.
- [x] Add `GroupBy::Opcode` and trim the opcode with first-non-whitespace and
  first-whitespace boundaries, reusing per-line accumulation:

  ```cpp
  const auto begin = inst.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos || inst[begin] == ';') continue;
  auto opcode = inst.substr(begin, inst.find_first_of(" \t\r\n", begin) - begin);
  ```
- [x] Add wave instance to the v2 digest, defaulting to -1 when absent, and
  coverage grouped by `(se,cu,simd)` with total groups and bounded rows.
- [x] Check GREEN and existing query/format regressions.

## Task 2: Granular occupancy summaries

Files: `src/cli/digest.{h,cpp}`, `digest_builder.cpp`, `format.{h,cpp}`,
`main.cpp`, `tests/cli/regression_test.cpp`, `tests/cli/occupancy_test.cpp`.

- [x] Add a real CLI fixture where two waves overlap over [0,1000):

  ```json
  {"0":[[0,1,3,0,1,0],[250,1,3,1,1,0],
         [750,1,3,0,0,0],[1000,1,3,1,0,0]]}
  ```

  Set the fixture wave's begin/end to 0/1000. Expected exact peak=2,
  mean=1.5, starts=2 for CU1/SIMD3. A same-timestamp handover must peak at 1.
- [x] Observe RED on `occupancy --by simd --se 0 --cu 1 --json`.
- [x] Build CU and SIMD summaries from occupancy records in analyze. Sweep
  grouped deltas, integrate only [t0,t1), retain unavailable state for invalid
  streams. Store compact rows, not extra binned arrays.
- [x] Add query filters, top cap/truncation disclosure and text/JSON output.
  Reject filters without a grouped mode before digest access.
- [x] Verify missing-vs-zero behavior and old digest compatibility; run GREEN.

## Task 3: Selected-wave wait query

Files: create `src/cli/wait_query.{h,cpp}`, extend `src/data/trace_loader.{h,cpp}`,
`src/cli/main.cpp`, root/test CMake registration and CLI regression tests.

- [x] Use a synthetic wave with one wait line executing with stall durations
  0, 4, 8. Expected count=3, mean=4, P95=8, max=8. Top 1 returns the 8-cycle
  occurrence, context=0 returns only that instruction. Add recorded dependency
  references and verify them without reconstructing expected values from code.
- [x] Observe RED using the new `wait` command against the old binary.
- [x] Add a selected-wave loader option. JSON constructs/validates only the
  selected wave; raw ATT preserves full listing discovery while skipping
  unrelated WaveInstance construction. Existing analyze uses its unchanged
  all-wave path. Optional CU is validated after loading real metadata.
- [x] In wait_query, validate wait opcode and selected line, enumerate matching
  dynamic tokens, calculate stable statistics, sort bounded occurrences, and
  resolve bounded dependency references from recorded data or buildWaitcnt.
- [x] Add CLI parsing and text/JSON formatting with stderr-only diagnostics.
  Test missing selectors, malformed numbers, absent wave, non-wait/unexecuted
  line, valid zero stall, context edges and dependency truncation.
- [x] Run the reference capture on both input paths with line1708 and
  SE0/SIMD3/slot0/wave0, checking count=512 and dependencies1705/1706.

## Final review and verification

- [x] Rebuild CLI with the user's preset. Configure tests from root with the
  existing decoder at `build/rocm-systems-build`, and bake current trace paths
  plus `RCV_CLI_BINARY=<repo>/build/rcv-cli` into CTest.
- [x] Run focused new tests then the bounded verbose CLI suite, checking actual
  per-case failures/skips. Inspect Qt-free linkage and selected ROCm path.
- [x] Analyze raw ATT to a new /tmp digest, exercise all three features and
  compare relevant metrics with the independent probe in
  `/tmp/rcv-att-analysis-CJxhpx/analysis-results.json`.
- [x] Review the full diff for lost strictness, unbounded output, clock/identity
  errors and unavailable-data semantics. Update README with real commands,
  units, coverage limitations and on-demand raw-decode cost.
- [x] Report commands, evidence and any remaining limitations.

## Execution record

User approval: "好 那就做這三個吧" after the three-feature proposal.
Ruling: retain the current checkout/build as explicitly requested by the user.
Ruling: keep raw ATT decoding capture-wide for stable line IDs; optimize wave
materialization and omit hidden-latency analysis for wait queries.
Interface review: Tasks 1/2 share digest/format/main and therefore run sequentially.
Task 3 shares main and adds an optional loader parameter, preserving default calls.

Implementation review correction: display Token::stall is uint16_t, whereas the
decoder accepts 24-bit stalls. The wait query now reads original selected-wave
instruction values into private full-width observations rather than widening
every shared Token. JSON is reread only for that wave; ATT uses its decoded
record. Contexts preserve source order at equal timestamps. JSON and in-memory
record regressions first reproduced 65536->0 and 70000->4464, then passed with
the exact original values. Added P95 N=20/21, 420 context-row maximum, nonzero
alignment and real JSON/raw ATT dependency checks. Read-only review recheck
reported no remaining Critical/Important findings.

Coverage clarification: capture-wide recorded starts differ from starts in the
digest's [t0,t1) window. Both are now stored/reported (reference: 8192 vs 8147).

Final verification (2026-09-10): preset CLI build succeeded. All 11 CLI CTest
targets passed: 95 GoogleTest cases, zero failures and zero skips. Final log:
`/tmp/rcv-drilldown-20260910-final-ctest.log`. Only CLI targets were built/run;
GUI and sanitizer suites are outside this verification.

Fresh raw ATT digest: `/tmp/rcv-drilldown-20260910-final-digest.json` (644129 bytes),
2521 lines, 128 instruction-traced waves, 16 traced SE/CU/SIMD groups, 1024
occupancy SIMD groups, all available and peak=2. Capture-wide starts=8192,
window starts=8147. Opcode s_wait_dscnt stall=56857762, exposed=29609428,
matching the independently derived reference exactly. Raw ATT and JSON wait
line1708/SE0/SIMD3/slot0/instance0 statistics and bounded contexts match:
count512, mean76.908203125, population stddev14.333274057546666, P95=120,
max152; dependency lines1705/1706. JSON stdout parsed independently with jq.

`ldd`/`readelf` verified Qt-free linkage and ROCm
`/home/AMD/chunylai/rocm7.15.0a20260712/lib/libamd_comgr.so.3`; `git diff --check`
passed. Branch remains rcv-cli at 63d7ccee2ec38af2a3ec4642ba4daec0723edcf5;
implementation, tests and docs remain uncommitted. No push, merge or PR.
