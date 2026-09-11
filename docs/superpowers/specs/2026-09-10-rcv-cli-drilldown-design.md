# ATT diagnostic drill-down

The user approved three additions: bounded wait-site inspection, opcode grouping
and capture coverage, and per-CU/SIMD occupancy summaries. The objective is useful
ATT diagnosis rather than completing the previously deferred six-feature list.

## Approach

Extend the existing Qt-free CLI and digest incrementally. A full instruction dump
would make the digest enormous; caching every wait occurrence would also add
analysis cost for unused queries. Instead, aggregate opcode and occupancy once,
then load a selected wave on demand for a wait query. JSON queries construct only
the selected wave; raw ATT still needs decoding to retain capture-wide ASM line
identities, but does not construct every WaveInstance or run hidden analysis.
Do not decode just one SE and assume its regenerated ASM indices match the full
capture. Input and code listing are expected to remain unchanged between queries.

## Interfaces

- `hotspot --by opcode`: group the first non-whitespace instruction token,
  excluding comments/empty listings. Sum the existing clamped per-line metrics.
  Preserve default exposed sorting, top 20 and maximum 1000 rows.
- `coverage [-d digest.json] [--top N] [--json]`: bounded rows of instruction-traced
  `(se,cu,simd)` groups with wave counts and wave-instance ranges. Report known CU
  counts separately from unknown identities. Summary includes compact coverage
  counts. Occupancy population is reported separately; never call it hardware
  utilization or extrapolate traced-wave costs to the whole GPU.
  Distinguish capture-wide recorded starts from starts in the digest window.
  Since wave instances are local to `(se,simd,slot)`, each group also reports
  distinct slot count and labels min/max as slot-local values aggregated across
  slots. Consumers use wave count, not range width, for the traced-wave total.
- `occupancy --by cu|simd [--se N] [--cu N] [--simd N] [--top N] [--json]`:
  exact peak and time-weighted mean resident waves from recorded occupancy events
  over the existing digest window `[t0,t1)`, plus wave starts in that window.
  Aggregate by `(se,cu)` or `(se,cu,simd)`; preserve encoded CU identities. Top
  defaults to 20, maximum 1000. Filters require grouped mode; a SIMD filter
  requires `--by simd`. Existing ungrouped occupancy command stays unchanged.
- `wait <trace> --line N --se N --simd N --slot N --wave N [--cu N]
  [--format json|att] [--top N] [--context N] [--json]`: all five line/wave
  selectors are required. Wave is the instance number within the selected
  SE/SIMD/slot, not the reused hardware slot. Optional CU verifies identity.
  Compute count/mean/population-stddev/P95-nearest-rank/max stall over all dynamic
  occurrences of the selected wait line. Show the top 3 occurrences by stall
  (maximum 20), each with 3 preceding/following dynamic instructions (maximum 10)
  and a bounded dependency list (maximum 32 entries, with truncation disclosed).
  At most 420 context rows are emitted. Numeric validation precedes trace I/O.

## Contracts

The digest becomes version 2 with additive wave-instance and grouped occupancy
fields. Version 1 remains readable; missing granular occupancy is unavailable,
not zero, and the user is told to re-run analyze. Do not store per-instruction
events. Preserve raw hidden components with read-time clamps, inclusive source
cost replication, include_idle=true, original per-SE bins, atomic output and
strict complete loading for analyze.

Wait durations are observed instruction stalls, NOT measured memory-completion latency.
Use full-width original JSON/decoded instruction fields: display Token::stall is
only 16 bits and cannot represent every valid decoder stall. Contexts are sorted
by aligned time, retaining recorded order at tied timestamps.
Preserve zero stalls, distinguish a missing/unexecuted line from a real zero,
reject non-wait instructions, and require a complete selected wave. Print loader
diagnostics on stderr so JSON stdout remains parseable. Report aligned cycle
timestamps. Existing recorded JSON wait dependencies or the gfx-specific ATT
wait-counter reconstruction are static line/iteration references; label their
provenance and do not invent per-token dependencies when correspondence is
unavailable. Empty dependency data is explicitly unavailable/empty.

Occupancy groups use grouped timestamp deltas, avoiding a false peak when a wave
ends exactly as another starts. Process pre-window events to establish initial
concurrency. The final half-open window excludes changes at t1. Unbalanced or
negative-count event streams must not silently claim trustworthy statistics;
mark the group unavailable. Measured zero-duration start/end pairs remain valid
available zero. Coverage and occupancy outputs disclose row truncation.

## Verification and scope

Use synthetic fixtures with independently calculated statistics for zero waits,
P95, top/context bounds, selector errors, dependency references, tied occupancy
events, pre-window activity and old digests. Preserve the original CLI suite and
add raw ATT/JSON checks on the existing reference capture: line 1708 has 512
occurrences in SE0/SIMD3/slot0/wave0 and waits on LDS lines 1705/1706; occupancy
groups peak at two resident waves per recorded CU/SIMD. Compare direct fields,
not decoder-version-dependent idle quantities.

Build with `cmake --preset rocm-dev` and `cmake --build --preset rocm-dev`, using
`build/rcv-cli`, ROCm `~/rocm7.15.0a20260712`, and LD_LIBRARY_PATH unset. Configure
tests from repository root, never tests/ cwd. Test trace paths are baked into
CTest properties. No GUI features, new counters, complete timeline export,
flamegraphs, kernel optimization, push, merge or PR are included.
