# An AI agent's guide to kernel bottleneck analysis with RCV

Use this guide to turn an existing AMD thread trace into an evidence-backed
kernel optimization proposal. The objective is useful performance analysis,
not exhaustive collection of every metric. Prefer a small, falsifiable experiment
over an expensive rewrite whose benefit is unclear.

This guide covers the Qt-free `rcv-cli`. Check the [CLI reference](../README.md#commands)
and your executable for supported commands. Supply your own capture and source;
no particular trace dataset or machine configuration is required. The worked
example is self-contained, and its numbers are not expected values for your trace.

## 1. Operating rules

- An analysis request authorizes offline inspection, not kernel edits, GPU runs,
  remote synchronization, builds, or control of other agents. Obtain authorization
  for those actions separately and follow the relevant repository instructions.
- Preserve the capture, code objects, source snapshots, and existing worktree
  changes. Put generated analysis outputs in a new directory outside the capture.
- Separate **observations**, **interpretations**, and **unverified hypotheses**.
  Missing counters or unknown provenance limit a claim; they are not measured zero.
- Every proposed change needs a source/ISA location, supporting evidence, a
  predicted effect, a correctness/resource risk, and an acceptance condition.
- Never claim a speedup from an attributed cycle percentage. Only controlled,
  repeated measurements of the actual workload establish performance improvement.

The normal sequence is:

```text
Capture/provenance check → analyze once → summary + coverage
  → hotspot + occupancy → selected wait summaries and contexts
  → source/ISA hypothesis → cost-ranked experiment proposal
```

Do not run every query on every instruction. Start with a few high-impact sites;
expand only when another query can distinguish competing explanations.

## 2. Establish the input and environment

Before decoding, inspect the capture configuration, stdout/stderr, and available
metadata. Record:

1. Exact kernel name, device/architecture, workload shape, data type, layout,
   mask and other relevant options. Resolve dimension ordering from the actual
   invocation rather than guessing from a shape tuple.
2. Warmup/repeat counts, validation mode, and whether timing was collected under
   profiling. A single profiled launch is not a clean benchmark baseline.
3. Selected dispatch/iteration, SE/CU/SIMD capture settings, buffer sizes, and
   warnings about full buffers, incomplete waves, or decode failures.
4. Kernel executable/code-object hashes, compiler version, source commit and
   dirty state. A matching filename or current HEAD alone is not provenance.
   Include dirty source/patch hashes and relevant policies/generated instances.
5. RCV commit/dirty state, executable and decoder identity, input format, ROCm
   dependencies, working directory, and the date/commands of this analysis.

A digest does not contain complete workload or decoder provenance. Save these
facts beside it. Verify numeric evidence and source-level interpretations directly
against the capture and matching code.

### Environment pitfalls

- Run from the RCV repository root, **not `tests/`**. A `token_def.json` in the
  current directory can override token definitions and alter analysis semantics.
- Normally unset `LD_LIBRARY_PATH` for the build-tree executable so its intended
  RUNPATH dependencies are used. A different ROCm library directory can select
  an incompatible or disassembly-disabled decoder. Inspect actual dependencies
  when necessary; do not change global environment settings to make a query run.
- Use a ROCm/decoder installation compatible with your capture and executable.
  See [build instructions](../README.md#building) if a compatible executable is
  missing. Do not rebuild as an implicit analysis step.
- Raw ATT requires a decoder, and a disassembly backend when relying on `.out`
  code objects. A JSON-only RCV build cannot decode raw ATT.
- `--format att` selects raw `.att` input; `--format json` selects a decoded
  trace directory, **not a digest file**. In mixed directories, auto-detection
  prefers `filenames.json`. Force a format and keep it consistent across queries.
- Keep the input, listing, and decoding setup unchanged between `analyze` and
  wait queries. Do not decode one SE in isolation and reuse capture-wide line IDs.

## 3. Minimal first pass: decode once, query the digest

The shell examples use Bash and `jq`. Replace the three path placeholders. Choose
an output parent outside the capture directory, using durable storage if the
results must be retained. Run the
blocks in order in the same shell; later drill-down blocks also require selectors
chosen from your results. Commands write only to the new output directory.

```bash
RCV_ROOT=/absolute/path/to/rocprof-compute-viewer
TRACE=/absolute/path/to/one/capture/trace
OUT_ROOT=/absolute/path/to/analysis-output
FORMAT=att                         # Use json for a decoded trace directory.
cd "$RCV_ROOT" || exit 1
test -x ./build/rcv-cli || exit 1
mkdir -p "$OUT_ROOT" || exit 1
OUT=$(mktemp -d "$OUT_ROOT/rcv-analysis-XXXXXX") || exit 1
rcv() { env -u LD_LIBRARY_PATH "$RCV_ROOT/build/rcv-cli" "$@"; }

rcv analyze "$TRACE" --format "$FORMAT" -o "$OUT/digest.json" \
  > "$OUT/analyze.stdout" 2> "$OUT/analyze.stderr" || exit 1
rcv summary -d "$OUT/digest.json" --json > "$OUT/summary.json" || exit 1
rcv coverage -d "$OUT/digest.json" --top 1000 --json > "$OUT/coverage.json" || exit 1
rcv hotspot -d "$OUT/digest.json" --by opcode --top 30 --json > "$OUT/opcodes.json" || exit 1
rcv hotspot -d "$OUT/digest.json" --sort exposed --top 20 --json > "$OUT/hotspots.json" || exit 1
rcv occupancy -d "$OUT/digest.json" --by cu --top 1000 --json > "$OUT/occupancy-cu.json" || exit 1
```

Read `analyze.stderr` as well as the exit status. Status `0` means command
success, `1` is a runtime failure, and `2` is a usage error. In this version,
invoking without a command (also `--help`) prints usage to stderr and returns `2`.
`analyze` refuses invalid/incomplete required data and publishes its digest
atomically. Success does not prove that the capture sampled the whole device
or that its profiled timing was uncontaminated.

Inspect the compact results first:

```bash
jq '{meta, cycles, coverage, occupancy, stall_reasons}' "$OUT/summary.json"
jq '.' "$OUT/coverage.json"
jq '.[0:10]' "$OUT/opcodes.json"
```

### First-pass questions and next actions

| Question | Evidence | Action |
|---|---|---|
| Is this the intended kernel and workload? | Capture logs, code-object identity, summary metadata | Resolve a mismatch before recommending source edits. |
| Is source attribution available? | `meta.lines_with_source` versus `meta.total_lines` | Use source hotspots when available; otherwise map ISA manually. |
| Which population was instruction-traced? | `coverage` groups and wave counts | State sample scope; do not extrapolate it to GPU utilization. |
| Which costs remain after estimated overlap? | Exposed hotspots, then stall/idle views | Select a few sites, distinguishing useful work from waits. |
| Is concurrency low, uneven, or tail-dominated? | Grouped and binned occupancy | Check launch geometry and resource limits before blaming occupancy. |
| Are required counters absent? | Capture configuration and counter artifacts | State unavailable; do not synthesize a bandwidth/latency measurement. |

`summary`'s occupancy peak is the maximum **bin mean**, not an instantaneous
peak. Grouped `occupancy --by cu|simd` reports exact event-derived peaks and
time-weighted means within `[t0,t1)`, subject to recorded-data availability.
Use ungrouped `occupancy` to inspect the binned time series.

For a suspect CU, set `SE` and `CU` to IDs reported by coverage, then query:

```bash
rcv occupancy -d "$OUT/digest.json" --by simd \
  --se "${SE:?choose SE}" --cu "${CU:?choose encoded CU}" --top 1000 --json
rcv occupancy -d "$OUT/digest.json" --se "$SE" --json
```

`coverage` and grouped occupancy default to only 20 rows. Check `truncated` and
matching-group counts. Even `--top 1000` can omit groups on a large GPU: partition
occupancy by SE/CU or inspect the complete stored group arrays after checking
the digest schema. Do not average a truncated list and call it the device mean.
Encoded CU IDs are not coordinates to renumber. Recorded occupancy starts and
instruction-traced waves are different populations; starts within the digest
window also differ from capture-wide starts.

## 4. Understand the numbers before ranking fixes

```text
issue   = latency - stall
total   = latency + idle
hidden  = estimated hidden idle + hidden stall + hidden issue
exposed = total - hidden
```

Hidden components are clamped to their corresponding costs on read. Prefer CLI
metrics over manually summing raw digest hidden fields without the same clamps.
Check `meta.hidden_latency_available` before interpreting hidden/exposed values.
Older version-1 digests can lack wave identities and granular occupancy; absent
information is unavailable, not zero. Regenerate only when the input is available
and the additional decode cost is justified.

RCV estimates hiding from busy pipes on the same SIMD, with a priority model
(WMMA above VALU, then memory, then scalar pipes). It is not a whole-GPU
critical-path reconstruction or a model of how scheduling changes after a fix.
Exposed attribution includes useful issue/execution work, not just wasted time.

Three rules prevent most incorrect conclusions:

- **Attribution is not elapsed time.** Costs summed over waves cannot be divided
  by dispatch duration to produce a utilization or speedup claim. Opcode shares
  need an explicit denominator. The text hotspot share uses total attribution;
  compute `opcode.exposed / summary.cycles.exposed` for an exposed-share claim.
- **A wait is not a stopwatch for its producer.** Observed wait stall is the
  remaining wait at that site, potentially involving several outstanding
  operations. It is not full VMEM/LDS completion latency. Idle attributed to a
  load is also not proof of that load's service time.
- **A hot instruction is not automatically a bad instruction.** WMMA includes
  necessary work. A startup instruction can inherit large idle attribution.
  Inspect hit counts, phase, dependencies and overlap before suggesting removal.

Additional views can help separate these cases:

```bash
rcv hotspot -d "$OUT/digest.json" --sort stall --top 20 --json
rcv hotspot -d "$OUT/digest.json" --sort idle --top 20 --json
# Only interpret mapped rows when source metadata is actually present:
rcv hotspot -d "$OUT/digest.json" --by source --top 20 --json
```

Source/inlining attribution can be inclusive. Do not sum source rows as though
they were a mutually exclusive wall-time partition.

## 5. Drill down from a hot wait to a repeated mechanism

Choose `LINE` from this capture's ASM hotspot results, not from an example or a
previous build. Inspect surrounding static instructions and then query all
occurrences in a bounded wave selection:

```bash
rcv asm -d "$OUT/digest.json" --around "${LINE:?choose wait ASM index}" --context 8 --json \
  > "$OUT/asm-site.json" || exit 1
rcv wait-summary "$TRACE" --format "$FORMAT" --line "$LINE" --max-waves 16 --json \
  > "$OUT/wait-pilot.json" 2> "$OUT/wait-pilot.stderr" || exit 1
jq '{matching_waves, selected_waves, truncated, waves}' "$OUT/wait-pilot.json"
```

`wait-summary` selects the first matching waves in ascending SE/SIMD/slot/instance
order, **not a random sample**. Its default is 16; its maximum is 128. If the
pilot is consequential, broaden it across the available SEs/slots/instances.
For more than 128 matching waves, use disjoint supported selectors and track
coverage; do not claim that one capped call analyzed every wave. This command
has no `--cu` filter: inspect the reported CU and use other selectors.

Next distinguish startup/tail effects from steady repetition. First read
`total_occurrences` per wave. Set `FIRST`/`LAST` to a justified range; do not
blindly use the FMHA example's `16-495` on another kernel.

```bash
rcv wait-summary "$TRACE" --format "$FORMAT" --line "$LINE" --max-waves 128 \
  --iterations "${FIRST:?choose first occurrence}-${LAST:?choose last occurrence}" --json \
  > "$OUT/wait-steady.json" 2> "$OUT/wait-steady.stderr" || exit 1

# A weighted mean across observed rows, not a mean of means with unequal counts:
jq '[.waves[] | select(.available and .statistics.count > 0) | .statistics] as $s
  | ($s | map(.count) | add // 0) as $n
  | {count: $n, mean: (if $n == 0 then null
      else ($s | map(.count * .mean) | add) / $n end)}' "$OUT/wait-steady.json"
```

The range is inclusive and zero-based, counting executions of **this static line
within each wave**, not source-loop IDs. Branches and unrolling can break a
one-to-one loop interpretation. Compare full and restricted distributions;
state the selection. Do not average per-wave P95s into a claimed pooled P95.
Unexecuted/empty-range rows have null statistics; actual zero stalls are valid
observations. `wait`, unlike `wait-summary`, errors on an empty selected range.

Select an observed row's `SE`, `SIMD`, `SLOT`, and `WAVE` instance to inspect:

```bash
rcv wait "$TRACE" --format "$FORMAT" --line "$LINE" \
  --se "${SE:?choose SE}" --simd "${SIMD:?choose SIMD}" \
  --slot "${SLOT:?choose slot}" --wave "${WAVE:?choose instance}" \
  --iterations "$FIRST-$LAST" --top 2 --context 5 --json \
  > "$OUT/wait-context.json" 2> "$OUT/wait-context.stderr" || exit 1
jq '{wave, statistics, dependencies, occurrences}' "$OUT/wait-context.json"
```

Wave identity is `(SE, SIMD, slot, instance)`; an instance number alone is not
global. `wait --cu N` optionally checks the selected wave's actual CU identity.
Use returned wave identities, not the width of an aggregated instance range.

Interpret the dynamic context carefully:

- Statistics cover the selected occurrences; displayed contexts are the
  **highest-stall** examples. Do not call a displayed maximum typical.
- Timestamps use aligned trace cycles. Context is time-ordered but can extend
  beyond the requested occurrence range.
- Dependency references have provenance, such as `inferred_att_waitcnt`.
  They are static references, not proven producer/consumer matches for each
  dynamic occurrence. ATT dependency iteration fields can be placeholders.
- A missing dependency list is unavailable evidence, not proof of no dependency.
- Raw ATT is decoded capture-wide once **per wait command**. Small `--top`,
  `--context`, or `--max-waves` bounds output/materialization, not raw decode I/O.
  JSON queries materialize selected waves, but do not validate unselected wave
  contents as `analyze` does. Do not change format silently just to save time.

## 6. Map the symptom to source and test competing explanations

Before naming a source-level bottleneck, trace this chain:

```text
Wait site → candidate producer(s) → memory address/register identity
  → consumer instruction → source operation and loop stage
```

Use captured ISA/code objects and matching source. Check effective address bases,
offsets, element widths, padding/strides, transpose modes, and buffer rotation.
For banked register naming, resolve selector state: on the FMHA gfx1250 example,
`s_set_vgpr_msb` makes the printed VGPR name insufficient without its physical
register interpretation. An ASM index is not a C++ line or an ISA text-file line.
Compiler scheduling, loop rotation and inlining can separate an operation from
its apparent source position. Even debug mappings are clues, not dynamic stages.

Generate a small ranked set of hypotheses, each with a distinguishing prediction:

| Observed pattern | Candidate explanation | Evidence needed before committing to a fix |
|---|---|---|
| Repeated LDS load → immediate wait → consumer | Too little load-to-use distance | Physical-register last uses and independent instructions available for overlap; then a targeted scheduling A/B. |
| LDS waits persist despite separation | LDS service/throughput or bank behavior | Lane addresses/layout and architecture-specific counters or controlled layout experiments. A wait alone does not prove bank conflicts. |
| Low, flat resident-wave plateau | Resource or launch residency limit | VGPR/SGPR/LDS allocation rules, launch constraints, spills and workgroup geometry. A small count reduction may not cross a threshold. |
| Sparse concurrency or a long tail | Insufficient work, imbalance, or dispatch effects | Occupancy time series, grid size and per-group behavior; not just mean occupancy. |
| Repeated barrier waits | Arrival skew or synchronization dependencies | Work before the barrier across participating waves; never remove a barrier from its cost alone. |
| Large matrix/VALU costs | Useful compute throughput or dependency chains | Issue/stall mix, instruction sequence, overlap, and supported utilization counters. High attribution is not proof of saturation. |
| Large VMEM/TDM waits | Exposed data arrival | Producer timing, prefetch distance, cache/bandwidth counters if available. Small waits do not rule out indirect memory limits. |

These are alternative explanations, not a checklist of changes to implement.
If the available capture cannot distinguish them, say what additional evidence
would help and whether acquiring it is worth the cost.

## 7. Know what the CLI does not provide

The current commands provide aggregates, residency statistics, bounded wait
contexts, and opcode comparisons. Do not invent commands for missing analyses:

- No CLI counter-based VMEM/LDS memory-completion latency distribution. Observed
  wait mean/stddev is a different metric; `SQ_INST_LEVEL_*` analysis requires
  appropriate captured counters and a supported interpretation/tool path.
- No CLI marker-scope flamegraph export. GUI flamegraph/counter functionality
  in the README is not automatically a CLI feature.
- No complete per-wave instruction timeline dump. The digest has wave identities
  and lifetimes, not a persistent per-instruction timeline cache; `wait` gives
  bounded context, not the entire dependency chain.
- No stall-reason reconstruction from plain thread trace. Reported stall reasons
  require supporting data (such as PC sampling); empty does not mean no stalls.
- No automatic source attribution without suitable source/debug metadata, and
  no automatic kernel speedup or root-cause proof.

Use a suitable existing tool or ask for additional capture data when needed.
Do not turn a kernel analysis into RCV feature development without agreement.

## 8. Compare an optimization only after establishing comparability

If the user authorizes implementation and new measurements, compare matched
captures using independently created digests:

```bash
rcv compare "${BEFORE_DIGEST:?set baseline digest}" "${AFTER_DIGEST:?set candidate digest}" \
  --top 20 --json > "$OUT/compare.json" || exit 1
```

`compare` matches all opcode aggregates before selecting its output rows. It does
not match operands or static instruction sites. Re-identify wait sites in each
new build; an ASM line index is not a stable identifier across builds.

- Delta means **after minus before**. Percent is relative to before and null for
  a zero baseline. Added/removed opcodes are labeled.
- Inspect raw and per-traced-wave costs. Per-wave normalization does not correct
  changed workload, loop counts, or sampling bias. Ranking uses absolute raw
  exposed delta, or stall delta if hidden analysis is unavailable on either side.
- Inspect warnings, population differences and time windows. Different occupancy
  bin counts change binned peak resolution. Missing data remains unavailable.
- Comparability stays `unverified` because digests lack workload/decoder provenance.
  Matching names do not override this: verify shape, kernel semantics, sampling,
  device, compiler/decoder, clocks, buffer completeness and external activity.
- Do not numerically mix absolute cycles/timings across servers. A within-server
  result can inform a qualitative hypothesis elsewhere, not serve as its baseline.

Require correctness, no unacceptable resource regression, and warmed, repeated,
preferably interleaved same-environment A/B timing outside ATT. Then use ATT to
explain the change. Check whether costs moved into another wait, idle, issue work,
or a different phase. Lower isolated wait cost is not sufficient acceptance.

## 9. Worked example: long-sequence CK FMHA

This example demonstrates how to distinguish attention's K and V operand waits,
not a recipe to apply K scheduling changes to every kernel. QK computes attention
scores from query/key tiles; PV multiplies normalized scores by value tiles.
All information needed to follow the reasoning is included below; no external
capture or report is needed.

- Workload: **B=1, H=8, S=32768, D=128** (batch, heads, sequence length, head
  dimension), BF16, dense/noncausal.
- gfx1250, query/key tile sizes M128/N64, 128 threads/block, 512 key-tile iterations.
- 128 instruction-traced waves across 16 SEs; occupancy separately records 8192
  starts. Instruction coverage is a selected sample, not full-device utilization.
- Peak residency: 8 waves/CU, 2/SIMD; metadata: 432 VGPR, 70 KiB LDS, zero spills.
- Twelve `s_wait_dscnt` sites contribute 50,366,038 stalled cycles, or 60.26% of
  traced stall. Their exposed attribution is 26,220,784, or 22.12% of all exposed.
- At one hot wait site, full-range mean wait is 74.4900 cycles. Across all 128 traced
  waves, occurrences 16–495 have mean 74.4909 over 61,440 observations. The
  repeated cost survives removing the first/last 16 occurrences of each wave.

To determine whether the waits belong to QK or PV, trace their producer addresses.
Here, K buffers use bases `0/0x4400` and group stride `4352 = 16 × 272` bytes;
V uses `0x8800/0xd000`, stride `4608 = 16 × 288`, and transpose LDS loads. The
hot loads match the K layout, and their physical destination registers feed
QK WMMA. These independent checks identify K waits; the wait opcode or its
position in the listing alone would not distinguish K from V.

The resulting first experiment is a narrow K-fragment scheduling change: increase
useful load-to-use distance without unsafe register reuse or large extra buffers.
The source already contains prefetch-style ordering, so emitted ISA must show
the intended overlap. Resource/live-range pressure is a guardrail; softmax and
reduction scheduling are secondary targets. LDS service/bank contributions remain
unresolved, not disproven. Check existing ping-pong buffers, padding, and
scheduling before proposing an optimization the kernel already implements.

Do **not** conclude “22% speedup,” “HBM bandwidth bound,” or “add Split-KV” from
these numbers. This prefill already has 2048 workgroups; a low-grid decode argument
does not transfer. The logged 4.200 ms is a single, non-warmed profiled launch.

For your own trace, derive `LINE`, wave selectors, and occurrence ranges from
its query results. The transferable lesson is the evidence chain and the
cost-aware experiment, not these particular costs, addresses, or resource counts.

## 10. Deliver a decision, not just a hotspot list

Use this compact report structure:

```text
Conclusion
  Most actionable observed bottleneck; confidence and unresolved alternatives.

Scope and provenance
  Workload/kernel/device; capture and source hashes; RCV/decoder; date;
  instruction sample versus occupancy population; missing data and truncation.

Evidence
  Command + output artifact + site/identity + metric/unit/denominator.
  Repetition across waves and steady occurrences; source/ISA mapping evidence.

Ranked recommendations (usually 1–3)
  Location → hypothesis → smallest experiment → predicted observation.
  Expected value versus effort; correctness/resource risk; acceptance/stop rule.

What not to do yet
  Expensive or weakly supported changes and the evidence needed to reconsider.

Validation and limitations
  Checks actually run; what remains unmeasured or inferred.
  Proposed correctness/timing/ATT validation, explicitly not claimed as completed.
```

Before handing off, verify:

- [ ] The invocation and source/code object refer to the intended workload.
- [ ] Every cited query succeeded; diagnostic logs and truncation were inspected.
- [ ] Percentages state their denominators and sampled population.
- [ ] Wait stalls, memory latency, residency, coverage and utilization are not conflated.
- [ ] Inferred dependencies and source labels are qualified and cross-checked.
- [ ] Startup/outlier contexts are not presented as typical steady-state behavior.
- [ ] No recommendation merely reapplies an optimization already in the baseline.
- [ ] Suggestions target evidence, respect user scope, and include a cost-aware stop rule.
- [ ] No unsupported speedup, hardware saturation, correctness or fresh-pass claim appears.
- [ ] Artifact paths and reproduction commands are recorded; temporary storage is disclosed.

Stop when there is a useful next experiment or when another query would not
change the recommendation. If the likely benefit is small or the required work
is large, explicitly recommend deferring it.
