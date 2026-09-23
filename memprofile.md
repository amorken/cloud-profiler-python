# Sampled allocation profiling implementation

## Context

Add opt-in `HEAP_ALLOC` profiling to the Python Cloud Profiler agent. Sample
successful allocations in CPython's `PYMEM_DOMAIN_MEM` and `PYMEM_DOMAIN_OBJ`
domains, attribute samples with the agent's existing native frame walker, and
upload an in-memory pprof profile through the existing Cloud Profiler flow.

This measures estimated allocation traffic, including allocations freed during
the interval. It does not measure live heap size. Do not write allocation events
to disk, start a helper process, or add a Memray/tracemalloc dependency. Keep
memory profiling disabled by default. The cleaned implementation baseline is
commit `305314c`.

## Tasks

### Plan and implementation setup

- [x] Review allocator, stack-walker, profile-builder, and upload APIs; settle
  the design and limits below.
- [x] Save this checklist in `memprofile.md` before implementation.

### Native allocator sampling

- [x] Add opt-in, process-lifetime MEM/OBJ allocator wrappers that preserve and
  delegate to the prior allocators and remain pass-through outside collection.
- [x] Add a thread-local reentrancy guard; preserve allocator return values,
  resulting `errno`, and Python exception state. Count only successful,
  outermost allocation requests; do not track frees or pointers.
- [x] Implement per-thread, per-collection byte-based Poisson sampling. For
  requested size `s` and mean interval `R`, use `p = -expm1(-max(s, 1) / R)`;
  selected requests contribute `1/p` estimated objects and `s/p` estimated
  bytes. Record large requests once and draw a fresh countdown at their end.
- [x] Check calloc multiplication safely; count successful reallocations using
  the full new size. Handle zero-byte and failed requests as specified below.
- [x] Identify the currently attached interpreter before touching sampler or
  collector state. Pass unsupported interpreter contexts through without
  profiling.
- [x] Add deterministic native tests for sampling, estimator weighting,
  allocator success/failure, calloc, realloc, zero size, and nested hooks.

### Bounded stack attribution and lifecycle

- [x] Reuse `PopulateFrames`; bound both visited and captured frames (128 max).
- [x] Copy bounded function names and filenames into owned native storage while
  code objects remain valid. Resolve line numbers with a bounded work budget;
  use line zero and mark truncated metadata when a budget is exceeded.
- [x] Add a dedicated weighted stack accumulator capped at 2,048 stacks and
  16 MiB native storage, with bounded probing and explicit unknown/overflow
  buckets that preserve estimated totals.
- [x] Preallocate before collection, use collection generations to handle
  interval boundaries, reject overlapping requests, and stop recording before
  profile export.
- [x] Detect allocator replacement; skip the affected collection and disable
  later memory profiles without interrupting CPU/WALL profiling.
- [x] Keep inherited collection inactive after fork. Support worker startup
  after fork; document that restarting an already-started inherited agent is
  unsupported in v1.
- [x] Test concurrent allocation collection, dynamic code-object lifetime,
  deep stacks, metadata truncation, capacity overflow, overlapping starts,
  allocator replacement, and fork inactivity.
- [ ] Run subinterpreter/no-GIL callback tests. Free-threaded builds now refuse
  initialization, and callbacks verify the main interpreter before sampling;
  no such runtime is available here, and the extension cannot be imported in a
  subinterpreter because its dependencies do not support that context.
- [x] Test collection-boundary transitions, shutdown with active hooks, and
  repeated storage reuse.

### Profile and agent integration

- [x] Add trailing `start()` options `enable_memory_profiling=False` and
  `memory_sampling_interval_bytes=4194304`; validate before installing hooks.
- [x] Register a `HEAP_ALLOC` profiler only after native initialization succeeds;
  reuse existing scheduling, duration, authentication, upload, and retry flow.
- [x] Encode `alloc_objects/count` and `alloc_space/bytes`, default to
  `alloc_space`, include the byte sampling period and actual interval metadata,
  and preserve leaf-first frame order. Do not multiply weighted values again.
- [x] Preserve symbol sharing in export and emit a valid empty profile for an
  empty interval.
- [x] Measure peak memory while exporting the bounded pprof profile.
- [x] Add bounded per-collection diagnostics for selected samples, attributed,
  unknown and overflow estimates, storage utilization, and elapsed/export time;
  include coverage/overflow in pprof comments and concise logs outside callbacks.
- [x] Add local gzip/protobuf tests, mocked scheduling/upload tests, and tests
  that CPU/WALL profiles continue to work when memory profiling is unavailable.
- [x] Document coverage, allocator-request and realloc semantics, sample
  weighting, limits, unsupported contexts, and the opt-in setting.

### Qualification

- [x] Build and test the available supported Linux CPython runtimes (3.12 and
  3.13); do not expand Python support in this work.
- [ ] Complete the supported Linux CPython matrix (3.7–3.13); 3.7–3.11 are not
  available in this environment.
- [x] Benchmark disabled, installed-but-idle, and active collection workloads,
  including allocation churn, multiple sizes, deep/varied stacks, and threads.
- [ ] Meet gates: zero hooks/storage when disabled; <=1% regression with inactive
  hooks; <=10% throughput regression in every active workload; <=1% average
  regression across scheduled collection; native collector storage <=16 MiB;
  bounded export with measured peak memory.
- [x] Validate CPU and HEAP_ALLOC uploads and decoded API readback in a controlled
  Cloud Profiler project (2026-09-23).
- [ ] Run an opt-in production canary.

## Implementation defaults and behavior

- Memory profiling is opt-in, requires the native CPU-profiler runtime support,
  and is owned by the main interpreter with the GIL enabled. CPU collection need
  not be active at the same time.
- Initial mean sampling interval is 4 MiB. Configuration must be a positive
  integer and remains constant for a collection; callers may request 512 KiB
  for denser samples.
- Cover only MEM/OBJ requests. RAW-domain and direct native allocation paths,
  Python freelists, live-heap profiling, subinterpreter support, and additional
  Python versions are out of scope.
- Count successful outermost requests. Failed calls do not count or advance the
  sampler. Successful reallocations count once at the full new requested size;
  frees pass through. Zero-size calls can count as requests and contribute zero
  bytes.
- Frame names and filenames are copied into bounded native storage (maximum
  1,024 UTF-8 bytes per field); visibly mark truncation. Bound line-table work
  and use line zero if it exceeds the budget.
- Sampling and callback failures must not change application allocation
  behavior. Fractional weights are accumulated and rounded once at export;
  invalid or overflowing output is rejected.
- On parent-start/fork, inherited collection remains inactive. Workers should
  start the agent after fork in v1.

## Validation record

- Runtimes available and tested: CPython 3.12.14 at
  `/tmp/cloud-profiler-py312/bin/python` and CPython 3.13.15 at
  `/tmp/cloud-profiler-py313/bin/python`. The system CPython 3.14.4 has no
  development headers. CPython 3.7–3.11 were not available, so the full
  supported 3.7–3.13 matrix is still unverified.
- Build commands and results:
  `CC=gcc CXX=g++ /tmp/cloud-profiler-py312/bin/python setup.py build_ext --inplace`
  succeeded on 3.12.14, and
  `CC=gcc CXX=g++ /tmp/cloud-profiler-py313/bin/python setup.py build_ext --inplace`
  succeeded on 3.13.15. Both reported the pre-existing
  `AsyncSafeTraceMultiset::Reset` class-memaccess warning; 3.13 also reported
  the pre-existing `PyImport_ImportModuleNoBlock` deprecation warning.
- Full available package tests:
  `TMPDIR=/tmp /tmp/cloud-profiler-py312/bin/python -m pytest -q` — 19 passed,
  1 skipped in 1.43s;
  `TMPDIR=/tmp /tmp/cloud-profiler-py313/bin/python -m pytest -q` — 19 passed,
  1 skipped in 1.46s. The skip is the bounded attribution test because the
  default sandbox denies `process_vm_readv` with EPERM. These tests include
  debug-allocator chaining, callback errno/exception preservation,
  overlapping-start rejection, fork inactivity, allocator replacement,
  a six-seed sampler estimator check, diagnostics-to-pprof comments, and
  mocked scheduling/upload behavior.
- Shared-walker CPU regression smoke:
  `TMPDIR=/tmp /tmp/cloud-profiler-py312/bin/python -m pytest -q tests/test_memory_profile.py::test_cpu_profile_smoke_after_shared_frame_walker_change`
  and the same command with `/tmp/cloud-profiler-py313/bin/python` — each
  reported `1 passed` (0.40s and 0.39s). Both parsed a gzip/protobuf CPU
  profile and checked CPU period/type metadata, accepting empty samples under
  the sandbox's `process_vm_readv` EPERM restriction.
- Approved unsandboxed bounded-attribution regression:
  `TMPDIR=/tmp /tmp/cloud-profiler-py312/bin/python -m pytest -q tests/test_memory_native.py::test_bounded_stack_metadata_and_capacity_overflow`
  and the same command with `/tmp/cloud-profiler-py313/bin/python` — each
  reported `1 passed in 0.31s`. It verified the 128-frame truncation marker,
  long function and filename markers, oversized line-table fallback to line
  zero, the 2,048-stack cap, overflow estimates, and the 65,536-frame export
  limit on both runtimes. Direct smoke results included a 200,012-byte line
  table and 305.7 overflow objects / 8,873.3 estimated bytes at capacity.
- Approved multi-thread and dynamic-code tests ran with the bounded test on both
  runtimes:
  `TMPDIR=/tmp /tmp/cloud-profiler-py312/bin/python -m pytest -q tests/test_memory_native.py::test_bounded_stack_metadata_and_capacity_overflow tests/test_memory_native.py::test_multithread_allocation_collection_attributes_worker_frames tests/test_memory_native.py::test_dynamic_code_objects_can_be_released_after_collection`
  and the same command with `/tmp/cloud-profiler-py313/bin/python` — each
  reported `3 passed` in about 0.7s. Worker stacks were attributed from four
  threads; a dynamically compiled function and its code object were released
  before export while the owned filename/function metadata remained available.
- Bounded export-memory stress:
  `PYTHONPATH=. /tmp/cloud-profiler-py312/bin/python benchmarks/measure_memory_export.py`
  reported 2,048/2,048 stacks, 6,143/65,536 exported frames, 13,246,576 native
  storage bytes, 1,923,577 bytes of additional Python `tracemalloc` peak during
  export, a 27,072-byte gzip profile, and 5.71ms native export time. The same
  command with `/tmp/cloud-profiler-py313/bin/python` measured 1,923,757 bytes
  additional Python peak, a 27,094-byte gzip profile, and 6.26ms export time.
  The tracer was installed before allocator-hook initialization so it remained
  in the delegated allocator chain. Native collector storage is reported
  separately because `tracemalloc` does not account for that fixed C allocation.
- Subinterpreter behavior remains unverified. Importing the package in a
  subinterpreter fails first because `cryptography.hazmat.bindings._rust` does
  not support that context; bypassing package initialization then fails because
  `_profiler` itself is not loadable in subinterpreters. The native API now
  checks the stored main interpreter before starting, checking availability,
  or stopping, but a callable subinterpreter regression test cannot reach it.
- Collector memory measurement on CPython 3.12.14:
  `PYTHONPATH=. /tmp/cloud-profiler-py312/bin/python -c 'from googlecloudprofiler import _profiler; print(_profiler._memory_test_storage_bytes()); assert _profiler._memory_initialize(524288); print(_profiler._memory_test_storage_bytes())'`
  printed `0` before opt-in and `13246560` bytes after initialization. This is
  below the 16 MiB cap. A test also asserts zero storage before initialization.
- In the default sandbox, a direct `process_vm_readv` probe returned `-1` with
  `errno=1 (EPERM)`, so a sandboxed collection cannot attribute Python stacks.
  With approved unsandboxed execution, the same probe copied 4 bytes
  successfully. The active benchmarks below ran unsandboxed and produced
  multiple attributed traces.
- Sampler smoke on CPython 3.12.14:
  `_memory_test_sampler(50, 100, 50)` selected with
  `p=0.3934693402873666`, object estimate `2.5414940825367984`, and byte
  estimate `127.07470412683992`; countdown 51 did not select; a zero-byte
  request used `max(size, 1)` for selection and estimated zero bytes. The
  callback-state helper returned
  `(exception_preserved=True, errno_preserved=True)`.
- Preliminary CPython 3.12.14 overhead run used 200 batches of 1,000 requests
  for small (32-byte), varied (32/256/4096-byte), and depth-32 allocation
  workloads, plus four concurrent 128-byte allocation threads. Three baseline
  and three installed-idle runs had median throughput respectively: small
  7.22M/8.11M ops/s, varied 6.34M/6.33M, deep 1.368M/1.345M, and four-thread
  16.75M/16.30M. This run does not establish the <=1% inactive-hook gate; deep
  and multithread medians exceeded it and run-to-run variation obscured smaller
  effects.
- Three active runs with stack attribution had median throughput small
  5.477M, varied 4.025M, deep 1.197M, and four-thread 10.284M ops/s. Relative
  to the baseline medians above, regressions were approximately 24%, 36%, 13%,
  and 39%. Median p99 batch latency (1,000 requests per batch) increased from
  0.153ms to 0.209ms for small churn, 0.175ms to 0.331ms for varied sizes,
  1.032ms to 1.102ms for deep stacks, and 0.089ms to 3.925ms in the four-thread
  case. These measurements fail the <=5% active throughput gate. The overhead
  qualification is not passed; do not treat this implementation as production
  ready. Idle results remain unproven against the <=1% gate.
- A later single-run small-allocation isolation after combining the active flag
  and sampler state and switching the countdown to integer arithmetic used the
  same 200 batches of 1,000 operations. Baseline / idle / active at the default
  512 KiB interval / active at `R=2^62` measured 8.18M / 8.05M / 6.04M / 6.21M
  ops/s. The very-large-interval run selected no samples and captured no stacks,
  yet remained 24.1% below baseline; the default active run was 26.2% below.
  The single-run idle difference was 1.5%, so it does not establish the <=1%
  idle gate. This isolates a material wrapper/sampler active-path floor from
  frame walking; optimization stopped because the 5% active gate remains far
  out of reach.
- Remaining unchecked work: subinterpreter/no-GIL runtime tests; CPython
  3.7–3.11 builds; the <=1% inactive and <=5% active throughput/p99 gates,
  scheduled average gate, controlled Cloud Profiler project validation, and an
  opt-in canary.

- Follow-up validation for the context and lifecycle changes: the native
  callback now checks GIL availability and the current interpreter before
  reading or advancing thread-local sampler state. Builds with
  `Py_GIL_DISABLED` refuse initialization because the global collector is
  protected by the GIL. Initialization also fails closed if the process-lifetime
  at-fork handler cannot be registered. Repeated collection boundary, storage
  reuse, and active-hook interpreter shutdown tests were added.
- Latest full validation after those changes: extension builds succeeded on
  CPython 3.12.14 and 3.13.15. Sandboxed full suites each reported 21 passed,
  3 skipped because `process_vm_readv` is denied for bounded, multi-threaded,
  and dynamic-code stack attribution. The authorized unsandboxed full suite
  reported 24 passed on each runtime, including those attribution tests and the
  new lifecycle/reuse/shutdown checks.
- Latest unsandboxed export stress: both interpreters reached 2,048/2,048
  stacks, 6,143/65,536 export frames, and 13,246,576 native storage bytes.
  Additional Python `tracemalloc` export peak was 1,923,579 bytes on 3.12 and
  1,923,725 bytes on 3.13; export took 5.98ms and 5.42ms respectively.
- Latest CPython 3.12 overhead check used three 200-batch baseline runs and
  three active runs with attributed stacks. Median baseline/active throughput
  was 8.23M/5.02M small ops/s, 6.33M/3.49M varied ops/s, 1.36M/1.14M deep
  ops/s, and 16.60M/9.25M four-thread ops/s: regressions of 39%, 45%, 16%, and
  44%. Median p99 batch latency rose from 0.152ms to 0.242ms (small), 0.189ms
  to 0.376ms (varied), 0.864ms to 1.352ms (deep), and 0.094ms to 5.206ms
  (four-thread). These local runs still fail the active overhead gates; the
  inactive-hook and scheduled-average gates remain unproven.

## Throughput optimization follow-up (2026-09-23)

- [x] Keep the no-sample allocation path inline and integer-only. Resolve the
  thread-local sampler once across the delegate call, defer probability math
  and stack capture until selection, and update the common 64-bit countdown
  without a 128-bit carry chain or high-word store.
- [x] Carry each thread's exponential residual between collection windows.
  The residual remains exponential by memorylessness, and the configured
  interval cannot change after hooks are installed. Selected samples still
  revalidate their captured collection generation before accessing shared
  collector state.
- [x] Cover the uncommon 128-bit countdown carry and borrow with deterministic
  native assertions, alongside the existing seeded estimator tests.
- [x] Add a threads-only benchmark case and a paired runner that randomizes
  baseline, installed-idle, and active configurations and reports per-repeat
  losses against that repeat's baseline.
- [x] Measure 512 KiB and 4 MiB with successful stack attribution on CPython
  3.12 and 3.13; additionally sweep small and threaded churn from 4 MiB to
  256 MiB on 3.12. Set the public default to 4 MiB as a measured compromise,
  and retain the 512 KiB override for callers who choose denser samples.
- [x] Build and run the full available test suites on CPython 3.12.14 and
  3.13.15 after the sampler changes.
- [ ] Meet the <=10% active-throughput gate for every workload. Current paired
  measurements still exceed it for small allocations and four-thread churn;
  do not claim the performance qualification is complete.

The qualification run used CPU affinity, seed `20260923`, randomized mode
order, successful native stack attribution, and three repeats. Both runtimes
used 1 million small, varied-size, and depth-32 allocations plus 4 million
allocations across four workers per run. Median paired throughput loss at the
512 KiB and 4 MiB intervals was:

| Runtime | Workload | 512 KiB | 4 MiB |
| --- | --- | ---: | ---: |
| 3.12 | Small allocations | 23.78% | 17.96% |
| 3.12 | Varied sizes | 27.30% | 14.76% |
| 3.12 | Depth-32 stacks | 7.35% | 4.32% |
| 3.12 | Four-thread churn | 24.06% | 17.70% |
| 3.13 | Small allocations | 15.60% | 14.93% |
| 3.13 | Varied sizes | 23.57% | 14.20% |
| 3.13 | Depth-32 stacks | 7.05% | 4.12% |
| 3.13 | Four-thread churn | 20.15% | 17.44% |

With real stack walking enabled, the 4 MiB interval improved the active
throughput median in every workload on both runtimes, with the largest gains
on varied sizes. The full interval sweep still did not reach the 10% gate.
Small churn on 3.12 remained 12.39% slower at 64 MiB with five selected
samples; threaded churn remained 14.53% slower at 64 MiB and 15.84% at 256
MiB, with ten and two selected samples respectively. Higher intervals stop
buying enough throughput to justify their loss of profile detail. The default
is therefore 4 MiB, while the 512 KiB override remains available.

The all-workload runs selected a median 5,643/654 samples at 512 KiB/4 MiB on
3.12 and 5,466/671 on 3.13, with 12/10 and 11/9 distinct attributed stacks.
The active throughput gate remains open, especially for small and threaded
churn; these measurements do not qualify the feature for production.

Full test suites after the final native build reported 23 passed and 4 skipped
on both CPython versions. On 3.12, skips were stack-attribution checks because
the sandbox blocks `process_vm_readv`; on 3.13, three had that restriction and
the subinterpreter test skipped because `_xxsubinterpreters` is unavailable in
that interpreter build. No-GIL runtime validation remains outstanding.

### Real Cloud Profiler API smoke test (2026-09-23)

- [x] Run a bounded local CPython 3.12.14 workload using the active gcloud login
  in project `kiloclaw-493fed13`; no VM or persistent credential file required.
- [x] Exercise the agent's normal `Client.config`, create, collect, and patch
  paths with CPU and HEAP_ALLOC continuously advertised.
- [x] Read back paginated API profiles, decode gzip/pprof, and compare sample
  types, totals, sampling periods, and workload stack attribution with the
  locally collected data. The API omits resource names in list responses,
  re-encodes profiles, and normalizes `CPU` to `cpu`; match the unique test
  deployment and decoded summaries instead of compressed bytes.
- [x] Run the native test suite outside the sandbox: **27 passed, no skips**.

Successful service: `heap-sampler-live-20260923-172955`.

| Type | Profile ID | Aggregated stack entries | Verified totals |
| --- | --- | --- | --- |
| CPU | `63ea00f2155d79f3` | 5, all attributed to workload | 999 samples, 9,990,000,000 ns |
| HEAP_ALLOC | `2c565c9964267184` | 7, all attributed to workload | 240,318,611 estimated objects, 254,235,467,758 estimated bytes |

Both collection windows were approximately ten seconds. Memory sampling used
4 MiB. Allocation totals measure traffic including freed objects, not retained
heap. Earlier harness debugging runs also uploaded profiles under separate
`heap-sampler-live-*` services. All test processes exited. This validates upload
and retrieval, not production overhead, estimator accuracy, or a long canary.

Reproduce explicitly (writes profiles to the selected project):

```sh
CC=gcc CXX=g++ /tmp/cloud-profiler-py312/bin/python setup.py build_ext --inplace
PYTHONPATH=. /tmp/cloud-profiler-py312/bin/python benchmarks/verify_cloud_profiles.py \
  --project kiloclaw-493fed13 \
  --gcloud /home/anders/install/google-cloud-sdk/bin/gcloud
```

The script uses an in-memory access token and has a five-minute process limit.
Native stack walking and API access must be permitted by the execution
environment. API scheduling determines how many profiles are collected before
both requested types have been seen. Local GCE metadata lookup warnings are
expected when running outside Google Compute Engine.

### Code review and simplification (2026-09-23)

Reviewed with the Brooks Lint skill, focusing on duplicated hot-path logic,
allocator composition, test fidelity, and trustworthy measurement.

- [x] Consolidate all MEM/OBJ allocation callbacks behind one inline request
  boundary, retaining recursion suppression, successful-request accounting,
  generation fencing, and errno/exception preservation. Compile-time delegates
  avoid introducing another runtime dispatch layer.
- [x] Preserve underlying allocator contexts and free callbacks directly;
  allocation profiling does not need to intercept frees. Validate chaining with
  pymalloc, debug, and malloc allocator modes.
- [x] Exercise the production sampler in deterministic tests instead of a
  separate copy of its probability calculation. Use local state for seeded
  sampler tests instead of temporarily modifying live thread-local state.
- [x] Reject duplicate benchmark intervals, retain raw measurements, include
  idle paired losses and p99 latency, and reject sampled runs without native
  stack attribution.
- [x] Strengthen cloud readback validation with a digest of resolved ordered
  stacks and their allocation values. Tolerate ID renumbering and sample
  merging; detect changed attribution even when aggregate totals match.
- [x] Correct documentation: the sampling interval is fixed after hook
  installation, not independently configurable for each collection.

Full unsandboxed suites: CPython 3.12.14 **33 passed**; CPython 3.13.15 **32
passed, 1 skipped** (`_xxsubinterpreters` is unavailable in that runtime).
The callback abstraction inlines into specialized allocator functions in the
GCC build; it does not add a callable `ProfileAllocation` layer.

Further optimization should measure TLS lookup, recursion-guard, delegation,
and countdown costs independently before changing synchronization or stack
walking. The <=10% active-throughput gate and production canary remain open.

The strengthened real API smoke test passed after the review under service
`heap-sampler-live-20260923-185254-0a752d63` in `kiloclaw-493fed13`:
CPU `373f3d8a5f1c8728` and HEAP_ALLOC `5d55c1c90a6020f4`. Server-returned
resolved stack/value digests matched the local profiles, along with totals,
schema, and sampling periods. The workload exited normally.

Final review performance comparison (CPython 3.12.14, CPU 0, five randomized
paired repeats, seed 20260923, 1,000 batches of 1,000 operations per case,
4 MiB interval, real native stack attribution):

| Workload | Before review: median active loss | After review: median active loss |
| --- | ---: | ---: |
| small | 15.94% | 9.94% |
| varied | 16.92% | 1.86% |
| deep | 2.37% | 3.05% |
| threads_4x | 17.71% | 12.79% |

The before/after sessions ran sequentially on the same host. Small, varied, and
threaded churn improved in these runs; deep-stack loss increased by 0.68
percentage points. These short comparisons do not isolate each change's
contribution, quantify confidence, or establish the <=10% gate.
Reproduction command (use the same runtime/build on both revisions):

```sh
/tmp/cloud-profiler-py312/bin/python benchmarks/compare_memory_throughput.py \
  --intervals 4194304 --repeats 5 --batches 1000 \
  --operations-per-batch 1000 --cpu 0
```

## Fixed-cost optimization, work package 1 (2026-09-23)

- [x] Add a reference/candidate benchmark runner that runs the same workload
  source against independently built extensions, randomizes modes and trees,
  calibrates each case, and checkpoints raw paired results after every repeat.
- [x] Add benchmark-only compile-time callback stages for delegation,
  generation checking, TLS/recursion guarding, and countdown sampling. Normal
  builds cannot expose the diagnostic-stage marker; builds for distribution
  reject the diagnostic flag.
- [x] Add a native MEM/OBJ malloc/calloc/realloc probe and verify the diagnostic
  extensions compile in isolated temporary trees.
- [x] Review measurement code and run both runtime test suites: 36 passed on
  Python 3.12; 35 passed and one unavailable-subinterpreter skip on 3.13.
- [x] Use the paired runner to qualify the next production callback revision;
  record the remaining failed performance gates in work package 2.

Initial pinned Python 3.12 native OBJ malloc probe, 50 million calls per run,
three hardware-counter repetitions: delegate-only stage 0 averaged 6.87 billion
user instructions and 1.94 billion cycles; TLS/recursion stage 2 averaged
8.97 billion instructions and 2.53 billion cycles; sampling stage 3 averaged
10.09 billion instructions and 2.64 billion cycles. Median native request
times were approximately 5.1, 7.3, and 8.0 ns. Stages 0-3 are diagnostic
builds with no meaningful exported allocation profiles and cannot qualify the
feature. These numbers support reducing work on the unselected callback path.

## Fixed-cost optimization, work package 2 (2026-09-23)

- [x] Move probability calculation, countdown renewal, and the entire selected
  request handler out of the common allocation callback. The GCC build reduces
  the OBJ malloc callback stack frame from 0x58 to 0x38 bytes and keeps one TLS
  resolution on the active path.
- [x] Review error-state handling, generation fencing, seeded sampling, and
  allocator chaining after extraction. The full unsandboxed suites remain 36
  passed on Python 3.12 and 35 passed/1 skipped on 3.13.
- [x] Reject the nonvolatile sampler pointer experiment: GCC emits multiple
  `__tls_get_addr` calls after delegation, whereas the retained volatile
  pointer resolves TLS once. No nonvolatile change is in production code.
- [x] Run ten-pair, three-second-per-mode Python 3.12 comparison against the
  committed reference on one pinned CPU. Active loss/95% upper bound:
  small 8.65/9.71%, varied 8.66/10.85%, deep 2.25/6.59%, and four-thread
  12.74/13.72%. Varied and threaded churn fail the active gate; installed-idle
  upper bounds also exceed 1% on small, varied, and threaded workloads.
- [x] Finish the mixed-size and import-after-workers tests: 38 passed on
  CPython 3.12 and 37 passed/1 unavailable-subinterpreter skip on 3.13. The
  ten-pair result leaves the performance gate open, so TLS alternatives are
  next.

## Fixed-cost optimization, work package 3 (2026-09-23)

- [x] Measure isolated x86-64 GNU2 TLS, a separate idle entry, register-held
  TLS, `-fno-plt`, `-O2`, cold countdown initialization, and branch-layout
  variants against the same optimized production extension. All variants were
  built under `/tmp`; none was applied to the distributable extension.
- [x] Review correctness and portability before selecting a TLS model. GNU2
  TLS cut the pinned native active OBJ malloc probe by roughly 0.2 ns/request,
  but [GCC documents additional runtime requirements](https://gcc.gnu.org/onlinedocs/gcc-4.9.4/gcc/i386-and-x86-64-Options.html)
  and [binutils documents silent failure against glibc without GNU2 TLS runtime
  fixes](https://lists.nongnu.org/archive/html/bug-binutils/2025-10/msg00023.html).
  Keep the conservative TLS model for a Python extension loaded on arbitrary
  supported Linux systems, including after application threads have started.
- [x] Reject the separate idle entry: with conservative TLS, a pinned native
  probe measured about 5.03 ns/request idle versus 5.15 ns current, but about
  8.04 ns/request active versus 7.58 ns current. The remaining compiler options
  and cold initialization did not show a repeatable improvement in both paths.
- [x] Run a five-pair, one-second-per-mode Python 3.12 screen of branch-layout
  hints. Candidate active loss medians were 10.26% small, 11.43% varied, 3.24%
  deep, and 11.91% four-thread; wide confidence bounds and candidate idle
  losses gave no basis to promote this variant. Keep the current implementation.
- [x] Review the package after measurement. Do not make a speculative callback
  edit merely to lower a single native microbenchmark. The ten-pair production
  qualification in work package 2 remains the current gate evidence.

The allocator [API contract](https://docs.python.org/3.13/c-api/memory.html)
requires an allocator installed after interpreter initialization to wrap the
existing one, and all allocator domains to be thread-safe. Consequently,
uninstalling the process-lifetime wrapper after each collection is not a safe
way to erase installed-idle cost. This package does not meet the <=1% idle or
<=10% active performance gates. Those remain open for a design change that
preserves allocation accounting and allocator chaining.

The delegation-only stage 0 gave a native OBJ malloc median of 4.39 ns/request
without hooks and 5.28 ns/request with its hook installed (five 20-million-call
repetitions, CPU 0). In paired Python workloads with ten two-second runs per
mode, idle-loss medians were 0.83% for small allocations and -1.47% for four
threads; both had large pair-to-pair variation. These results identify wrapper
cost in the native probe but **do not** prove that a 1% idle gate is impossible
for whole Python workloads. Diagnostic stage 0 does not produce a valid memory
profile and is not a feature candidate.

After work package 2, the normal CPython 3.12 extension again uploaded CPU and
HEAP_ALLOC profiles through the real Cloud Profiler API in project
`kiloclaw-493fed13`, service `heap-sampler-live-20260923-203547-fc8e4e7d`.
Server readback decoded and matched the locally recorded resolved stack/value
digests for CPU profile `51e609a4d335cdd2` and HEAP_ALLOC profile
`5704f51b4b6822d6`. The latter had positive workload-attributed samples and
the expected 4 MiB period. This confirms end-to-end behavior, not the open
throughput gates.

## Runtime overhead follow-up (2026-09-23)

- [x] Record matched native compiler/linker commands, source hashes, extension
  hashes, and interpreter identity for reference and candidate builds. The
  paired runner rejects stale or mismatched builds and checks the exact loaded
  extension.
- [x] Add one- and four-worker versions of the same allocation loop and a
  repeated eight-frame stack workload. Keep the depth-32 workload for the
  deeper walker case.
- [x] Review the measurement harness and run an identical-source smoke
  comparison with real stack attribution on Python 3.12. Both sides used the
  same production flags, including frame pointers and stack protection. Short
  two-pair measurements varied substantially and are not gate evidence.
- [ ] Move first-use countdown initialization and high-word arithmetic out of
  the common callback, then test and measure a matched build.
- [ ] Add a GIL-held synchronous frame walker for allocation samples on 3.12
  and 3.13, retaining the signal-safe walker for CPU signals and other versions.
- [ ] Add bounded collection-local resolved metadata reuse without retaining
  code objects or changing attribution on cache miss or exhaustion.
- [ ] Document an allocator hook-switching feasibility decision from CPython
  source without changing hook installation in this iteration.
- [ ] Run full tests, export stress, and matched ten-pair throughput and p99
  qualification on 3.12 and 3.13; repeat real Cloud Profiler API readback.
