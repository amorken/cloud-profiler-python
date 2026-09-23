// Copyright 2018 Google LLC
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

#include <Python.h>

#include <cstdint>
#include <cstddef>
#include <time.h>

#include "clock.h"
#include "memory_profiler.h"
#include "profiler.h"

namespace {
#ifdef GCLOUDPROFILER_BENCH_STAGE
PyObject* MemoryDiagnosticStage(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  return PyLong_FromLong(GCLOUDPROFILER_BENCH_STAGE);
}
#endif

PyObject* ProfileCPU(PyObject* self, PyObject* args) {
  uint64_t duration_nanos = 0;
  uint64_t period_msec = 0;
  if (!PyArg_ParseTuple(args, "LL", &duration_nanos, &period_msec)) {
    return nullptr;
  }

  CPUProfiler p(duration_nanos, period_msec * kNanosPerMilli);
  return p.Collect();
}

PyObject* InitializeMemory(PyObject* self, PyObject* args) {
  (void)self;
  unsigned long long sampling_interval_bytes = 0;
  if (!PyArg_ParseTuple(args, "K", &sampling_interval_bytes)) {
    return nullptr;
  }
  return PyBool_FromLong(
      InitializeMemoryProfiler(sampling_interval_bytes) ? 1 : 0);
}

PyObject* StartMemory(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  return PyBool_FromLong(StartMemoryProfile() ? 1 : 0);
}

PyObject* IsMemoryAvailable(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  return PyBool_FromLong(MemoryProfilerAvailable() ? 1 : 0);
}

PyObject* MemoryStorageBytes(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  return PyLong_FromSize_t(MemoryProfilerStorageBytes());
}

PyObject* StopMemory(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  return StopMemoryProfile();
}

PyObject* TestMemorySampler(PyObject* self, PyObject* args) {
  (void)self;
  unsigned long long size = 0;
  unsigned long long interval = 0;
  double countdown = 0;
  if (!PyArg_ParseTuple(args, "KKd", &size, &interval, &countdown)) {
    return nullptr;
  }
  bool selected = false;
  double probability = 0;
  double objects = 0;
  double bytes = 0;
  if (!TestMemorySampling(static_cast<size_t>(size), interval, countdown,
                          &selected, &probability, &objects, &bytes)) {
    PyErr_SetString(PyExc_ValueError,
                    "interval must be positive and countdown finite");
    return nullptr;
  }
  return Py_BuildValue("(Oddd)", selected ? Py_True : Py_False, probability,
                       objects, bytes);
}

PyObject* TestMemoryCallbackState(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  bool exception_preserved = false;
  bool errno_preserved = false;
  if (!TestMemoryCallbackPreservation(&exception_preserved,
                                     &errno_preserved)) {
    PyErr_SetString(PyExc_RuntimeError,
                    "memory callback state test requires active collection");
    return nullptr;
  }
  return Py_BuildValue("(NN)", PyBool_FromLong(exception_preserved),
                       PyBool_FromLong(errno_preserved));
}

PyObject* TestMemoryFastCallbackState(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  bool exception_preserved = false;
  bool errno_preserved = false;
  bool sample_count_unchanged = false;
  if (!TestMemoryFastPathPreservation(&exception_preserved,
                                      &errno_preserved,
                                      &sample_count_unchanged)) {
    PyErr_SetString(PyExc_RuntimeError,
                    "memory fast-path test requires active collection");
    return nullptr;
  }
  return Py_BuildValue("(NNN)", PyBool_FromLong(exception_preserved),
                       PyBool_FromLong(errno_preserved),
                       PyBool_FromLong(sample_count_unchanged));
}

PyObject* TestMemoryCountdown(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  return PyBool_FromLong(TestMemoryCountdownArithmetic());
}

PyObject* TestMemoryAllocatorCalls(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;

  void* allocation = PyMem_Malloc(32);
  const bool malloc_succeeded = allocation != nullptr;
  void* resized = allocation == nullptr ? nullptr : PyMem_Realloc(allocation, 96);
  const bool realloc_succeeded = resized != nullptr;
  if (allocation != nullptr && resized == nullptr) PyMem_Free(allocation);

  void* zero_size = PyMem_Malloc(0);
  const bool zero_size_succeeded = zero_size != nullptr;
  void* zero_product = PyMem_Calloc(0, 64);
  const bool zero_product_succeeded = zero_product != nullptr;
  void* calloc_allocation = PyMem_Calloc(3, 17);
  const bool calloc_succeeded = calloc_allocation != nullptr;
  void* object_allocation = PyObject_Malloc(48);
  const bool object_succeeded = object_allocation != nullptr;
  void* failed_allocation = PyMem_Malloc(SIZE_MAX);
  const bool huge_failed = failed_allocation == nullptr;
  void* overflow_calloc = PyMem_Calloc(SIZE_MAX, 2);
  const bool overflow_calloc_failed = overflow_calloc == nullptr;

  PyMem_Free(resized);
  PyMem_Free(zero_size);
  PyMem_Free(zero_product);
  PyMem_Free(calloc_allocation);
  PyMem_Free(failed_allocation);
  PyMem_Free(overflow_calloc);
  PyObject_Free(object_allocation);
  return Py_BuildValue(
      "(NNNNNNNN)", PyBool_FromLong(malloc_succeeded),
      PyBool_FromLong(realloc_succeeded), PyBool_FromLong(calloc_succeeded),
      PyBool_FromLong(zero_size_succeeded),
      PyBool_FromLong(zero_product_succeeded),
      PyBool_FromLong(object_succeeded), PyBool_FromLong(huge_failed),
      PyBool_FromLong(overflow_calloc_failed));
}

PyObject* TestMemoryNativeChurn(PyObject* self, PyObject* args) {
  (void)self;
  int domain = 0;
  int operation = 0;
  unsigned long long count = 0;
  unsigned long long size = 0;
  if (!PyArg_ParseTuple(args, "iiKK", &domain, &operation, &count, &size)) {
    return nullptr;
  }
  if ((domain != 1 && domain != 2) || operation < 0 || operation > 2 ||
      count == 0 || size > SIZE_MAX || size == 0) {
    PyErr_SetString(PyExc_ValueError, "invalid native churn parameters");
    return nullptr;
  }
  void* (*allocate)(size_t) =
      domain == 1 ? PyMem_Malloc : PyObject_Malloc;
  void* (*allocate_zeroed)(size_t, size_t) =
      domain == 1 ? PyMem_Calloc : PyObject_Calloc;
  void* (*resize)(void*, size_t) =
      domain == 1 ? PyMem_Realloc : PyObject_Realloc;
  void (*release)(void*) = domain == 1 ? PyMem_Free : PyObject_Free;

  struct timespec start, end;
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
    return PyErr_SetFromErrno(PyExc_OSError);
  }
  for (unsigned long long i = 0; i < count; ++i) {
    void* block = operation == 1 ? allocate_zeroed(1, size) :
                  allocate(operation == 2 ? size / 2 + 1 : size);
    if (block == nullptr) {
      PyErr_NoMemory();
      return nullptr;
    }
    if (operation == 2) {
      void* resized = resize(block, size);
      if (resized == nullptr) {
        release(block);
        PyErr_NoMemory();
        return nullptr;
      }
      block = resized;
    }
    release(block);
  }
  if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
    return PyErr_SetFromErrno(PyExc_OSError);
  }
  const unsigned long long elapsed =
      (static_cast<unsigned long long>(end.tv_sec) - start.tv_sec) *
          1000000000ULL + end.tv_nsec - start.tv_nsec;
  return PyLong_FromUnsignedLongLong(elapsed);
}

PyObject* TestMemoryNestedHookCall(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  return PyBool_FromLong(TestMemoryNestedHook() ? 1 : 0);
}

PyObject* TestMemorySamplingSequence(PyObject* self, PyObject* args) {
  (void)self;
  unsigned long long size = 0;
  unsigned long long interval = 0;
  unsigned long long requests = 0;
  unsigned long long seed = 0;
  if (!PyArg_ParseTuple(args, "KKKK", &size, &interval, &requests, &seed)) {
    return nullptr;
  }
  uint64_t selected = 0;
  double objects = 0;
  double bytes = 0;
  if (!RunMemorySamplingSequence(static_cast<size_t>(size), interval,
                                 requests, seed, &selected, &objects, &bytes)) {
    PyErr_SetString(PyExc_ValueError,
                    "interval and request count must be positive");
    return nullptr;
  }
  return Py_BuildValue("(Kdd)",
                       static_cast<unsigned long long>(selected), objects,
                       bytes);
}

PyObject* TestMemoryMixedSamplingSequence(PyObject* self, PyObject* args) {
  (void)self;
  unsigned long long interval = 0;
  unsigned long long requests = 0;
  unsigned long long seed = 0;
  if (!PyArg_ParseTuple(args, "KKK", &interval, &requests, &seed)) {
    return nullptr;
  }
  uint64_t selected = 0;
  double objects = 0;
  double bytes = 0;
  if (!RunMemoryMixedSamplingSequence(interval, requests, seed, &selected,
                                      &objects, &bytes)) {
    PyErr_SetString(PyExc_ValueError,
                    "interval and request count must be positive");
    return nullptr;
  }
  return Py_BuildValue("(Kdd)",
                       static_cast<unsigned long long>(selected), objects,
                       bytes);
}

PyObject* TestMemoryAllocatorReplacement(PyObject* self, PyObject* args) {
  (void)self;
  if (!PyArg_ParseTuple(args, "")) return nullptr;
  return PyBool_FromLong(TestReplaceMemoryAllocator() ? 1 : 0);
}

PyMethodDef ProfilerMethods[] = {
#ifdef GCLOUDPROFILER_BENCH_STAGE
    {"_memory_diagnostic_stage", MemoryDiagnosticStage, METH_VARARGS,
     "Benchmark-only native callback stage."},
#endif
    {"profile_cpu", ProfileCPU, METH_VARARGS, "A function for CPU profiling."},
    {"_memory_initialize", InitializeMemory, METH_VARARGS,
     "Initialize sampled allocation profiling."},
    {"_memory_start", StartMemory, METH_VARARGS,
     "Start a sampled allocation profile."},
    {"_memory_available", IsMemoryAvailable, METH_VARARGS,
     "Check whether allocator hooks remain available."},
    {"_memory_test_storage_bytes", MemoryStorageBytes, METH_VARARGS,
     "Test-only collector storage size."},
    {"_memory_stop", StopMemory, METH_VARARGS,
     "Stop and export a sampled allocation profile."},
    {"_memory_test_sampler", TestMemorySampler, METH_VARARGS,
     "Test-only deterministic sampler helper."},
    {"_memory_test_callback_state", TestMemoryCallbackState, METH_VARARGS,
     "Test-only allocator callback state check."},
    {"_memory_test_fast_callback_state", TestMemoryFastCallbackState,
     METH_VARARGS, "Test-only unsampled allocator callback state check."},
    {"_memory_test_countdown", TestMemoryCountdown, METH_VARARGS,
     "Test-only countdown carry and borrow check."},
    {"_memory_test_allocator_calls", TestMemoryAllocatorCalls, METH_VARARGS,
     "Test-only allocator operation check."},
    {"_memory_test_native_churn", TestMemoryNativeChurn, METH_VARARGS,
     "Test-only timed native allocator calls."},
    {"_memory_test_nested_hook", TestMemoryNestedHookCall, METH_VARARGS,
     "Test-only nested allocator callback check."},
    {"_memory_test_sequence", TestMemorySamplingSequence, METH_VARARGS,
     "Test-only deterministic seeded sampler sequence."},
    {"_memory_test_mixed_sequence", TestMemoryMixedSamplingSequence,
     METH_VARARGS, "Test-only mixed-size seeded sampler sequence."},
    {"_memory_test_replace_allocator", TestMemoryAllocatorReplacement,
     METH_VARARGS, "Test-only allocator replacement trigger."},
    {nullptr, nullptr, 0, nullptr} /* Sentinel */
};

struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT, "_profiler",           /* name of module */
    "Google Cloud Profiler C++ extension module", /* module documentation */
    -1, ProfilerMethods};
}  // namespace

PyMODINIT_FUNC PyInit__profiler(void) { return PyModule_Create(&moduledef); }
