// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GOOGLECLOUDPROFILER_SRC_MEMORY_PROFILER_H_
#define GOOGLECLOUDPROFILER_SRC_MEMORY_PROFILER_H_

#include <Python.h>

#include <cstddef>
#include <cstdint>

// Installs process-lifetime MEM and OBJ allocator wrappers on the main
// interpreter. Returns false when memory profiling is unsupported or the
// allocator hooks have previously been replaced.
bool InitializeMemoryProfiler(uint64_t sampling_interval_bytes);
bool MemoryProfilerAvailable();
size_t MemoryProfilerStorageBytes();

// Starts one non-overlapping collection. The collector storage is allocated
// before the wrappers are installed.
bool StartMemoryProfile();

// Stops recording before exporting owned traces. The returned mapping has
// leaf-first frame tuples mapped to (estimated objects, estimated bytes).
PyObject *StopMemoryProfile();

// Deterministic sampler hook used by the native correctness tests.
bool TestMemorySampling(size_t requested_size, uint64_t interval,
                        double countdown, bool *selected,
                        double *probability, double *objects,
                        double *bytes);

// Exercises one wrapped request with a pre-existing Python exception and
// errno value. Used by native correctness tests only while collection runs.
bool TestMemoryCallbackPreservation(bool *exception_preserved,
                                   bool *errno_preserved);
bool TestMemoryNestedHook();
bool RunMemorySamplingSequence(size_t requested_size, uint64_t interval,
                               uint64_t requests, uint64_t seed,
                               uint64_t *selected, double *objects,
                               double *bytes);
bool TestReplaceMemoryAllocator();

#endif  // GOOGLECLOUDPROFILER_SRC_MEMORY_PROFILER_H_
