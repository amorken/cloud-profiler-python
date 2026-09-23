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

#include "memory_profiler.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "populate_frames.h"
#include "stacktraces.h"

#if defined(GCLOUDPROFILER_BENCH_STAGE) && \
    (GCLOUDPROFILER_BENCH_STAGE < 0 || GCLOUDPROFILER_BENCH_STAGE > 3)
#error "invalid diagnostic allocation stage"
#endif

namespace {

// These limits also bound work performed by an allocation callback. The
// fixed tables and the string arena occupy about 13 MiB in total.
const uint32_t kMaxStacks = 2048;
const uint32_t kStackHashSlots = 4096;
const uint32_t kMaxStrings = 65536;
const uint32_t kStringHashSlots = 131072;
const uint32_t kMaxStringBytes = 8 * 1024 * 1024;
const uint32_t kMaxStringLength = 1024;
const uint32_t kMaxExportFrames = 65536;
const uint32_t kMaxLineLookups = 1000000;
const uint32_t kMaxLineTableBytes = 64 * 1024;
const uint32_t kMaxLineBytesPerCollection = 16 * 1024 * 1024;
const uint32_t kMaxProbe = 16;
const char kTruncatedMarker[] = "[truncated]";
const char kLineTruncatedMarker[] = "[line-truncated]";
const char kStackTruncatedMarker[] = "[stack-truncated]";
const char kUnknownName[] = "[Unknown]";
const char kOverflowName[] = "[Overflow]";

struct StoredFrame {
  uint32_t name_id;
  uint32_t filename_id;
  int32_t line;
};

struct StackEntry {
  uint64_t hash;
  long double objects;
  long double bytes;
  uint16_t frame_count;
  StoredFrame frames[kMaxFramesToCapture];
};

struct StringEntry {
  uint64_t hash;
  uint32_t offset;
  uint16_t length;
};

struct CollectorData {
  StackEntry stacks[kMaxStacks];
  uint16_t stack_slots[kStackHashSlots];  // index + 1; zero means empty
  StringEntry strings[kMaxStrings];
  uint32_t string_slots[kStringHashSlots];  // index + 1; zero means empty
  char string_arena[kMaxStringBytes];
  uint32_t stack_count;
  uint32_t total_export_frames;
  uint32_t string_count;
  uint32_t string_bytes;
  uint32_t line_lookups;
  uint32_t line_bytes_visited;
  bool invalid_estimate;
  uint64_t selected_samples;
  long double unknown_objects;
  long double unknown_bytes;
  long double overflow_objects;
  long double overflow_bytes;
};

static_assert(sizeof(CollectorData) <= 16 * 1024 * 1024,
              "native memory profile storage must stay within 16 MiB");

CollectorData *g_data = nullptr;
PyMemAllocatorEx g_mem_allocator;
PyMemAllocatorEx g_obj_allocator;
// Used only by the allocator-replacement subprocess test. This remains a
// proper post-init wrapper around the current allocator, with process-lifetime
// storage for both the delegate and the context passed to the wrapper.
PyMemAllocatorEx g_replacement_underlying_allocator;
PyMemAllocatorEx g_replacement_allocator;
PyInterpreterState *g_main_interpreter = nullptr;
uint64_t g_sampling_interval = 0;
bool g_hooks_installed = false;
std::atomic<bool> g_hooks_disabled(false);
bool g_after_fork = false;
// A nonzero collection generation also means collection is active. Wrappers
// snapshot it once per allocation; selected samples recheck it before touching
// the collector so work cannot cross a collection boundary.
std::atomic<uint64_t> g_active_generation(0);
uint64_t g_next_generation = 1;
std::atomic<uint64_t> g_seed(0x4d595df4d0f33173ULL);
timespec g_collection_start_monotonic = {0, 0};
int64_t g_collection_start_wall_ns = 0;

struct ThreadSampler {
  bool in_hook;
  uint64_t interval;
  uint64_t rng;
  typedef unsigned __int128 Countdown;
  uint64_t countdown_low;
  uint64_t countdown_high;
};

typedef ThreadSampler::Countdown Countdown;

// This is deliberately a trivial zero-initialized TLS object. A constructor
// here makes compilers emit a per-thread initialization guard on every access
// from the allocator hooks.
thread_local ThreadSampler g_thread_sampler = {};

inline ThreadSampler *CurrentThreadSampler() { return &g_thread_sampler; }

uint64_t NextRandom(ThreadSampler *sampler) {
  if (sampler->rng == 0) {
    uint64_t seed = g_seed.fetch_add(0x9e3779b97f4a7c15ULL,
                                     std::memory_order_relaxed);
    // Mix a per-thread sequence value without asking the runtime for a thread
    // identifier, which can call into platform libraries from the hook.
    seed ^= reinterpret_cast<uintptr_t>(sampler);
    if (seed == 0) seed = 0x2545f4914f6cdd1dULL;
    sampler->rng = seed;
  }
  uint64_t x = sampler->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  sampler->rng = x;
  return x * 0x2545f4914f6cdd1dULL;
}

double UniformOpen01(ThreadSampler *sampler) {
  // Use an exactly representable denominator greater than every numerator.
  // 2^53 + 1 rounds to 2^53 as a double and could therefore return 1.0.
  const uint64_t mantissa = (NextRandom(sampler) >> 11) + 1;
  return static_cast<double>(mantissa) / 9007199254740994.0;
}

Countdown DrawCountdown(uint64_t interval, ThreadSampler *sampler) {
  const int saved_errno = errno;
  const long double draw =
      -static_cast<long double>(interval) *
      std::log(static_cast<long double>(UniformOpen01(sampler)));
  errno = saved_errno;
  const long double max_exclusive = std::ldexp(1.0L, 128);
  const Countdown max_countdown = ~static_cast<Countdown>(0);
  if (!std::isfinite(static_cast<double>(draw)) || draw >= max_exclusive) {
    return max_countdown;
  }
  const long double rounded = std::ceil(draw);
  if (rounded >= max_exclusive) return max_countdown;
  if (rounded <= 1.0L) return 1;
  return static_cast<Countdown>(rounded);
}

// Keep the exponential residual across collection windows. It remains
// exponential by memorylessness, and the configured interval is immutable.
__attribute__((noinline))
bool ConsumeCountdownSlow(uint64_t size, ThreadSampler *sampler) {
  uint64_t countdown_low = sampler->countdown_low;
  uint64_t countdown_high = sampler->countdown_high;
  if (countdown_low == 0 && countdown_high == 0) {
    // Zero is reserved for a thread that has not sampled its first allocation.
    sampler->interval = g_sampling_interval;
    const Countdown countdown = DrawCountdown(sampler->interval, sampler);
    sampler->countdown_low = static_cast<uint64_t>(countdown);
    sampler->countdown_high = static_cast<uint64_t>(countdown >> 64);
    countdown_low = sampler->countdown_low;
    countdown_high = sampler->countdown_high;
  }
  if (countdown_low > size) {
    // A new countdown can also fit in one word after first use.
    sampler->countdown_low = countdown_low - size;
    return false;
  }
  if (countdown_high != 0) {
    const uint64_t remaining_low = countdown_low - size;
    if (countdown_low < size) --countdown_high;
    sampler->countdown_low = remaining_low;
    sampler->countdown_high = countdown_high;
    return false;
  }
  return true;
}

inline bool ConsumeCountdownBytes(uint64_t size, ThreadSampler *sampler) {
  const uint64_t countdown_low = sampler->countdown_low;
  if (countdown_low > size) {
    // The ordinary request needs only one read, compare, and store.
    sampler->countdown_low = countdown_low - size;
    return false;
  }
  return ConsumeCountdownSlow(size, sampler);
}

// Keep transcendental math and countdown renewal outside the callback's
// ordinary path. An explicitly separate function also avoids selected-event
// register spills and stack setup on most unselected requests.
__attribute__((noinline))
long double FinishSelectedRequest(size_t requested_size,
                                  ThreadSampler *sampler) {
  const uint64_t effective_size =
      requested_size == 0 ? 1 : static_cast<uint64_t>(requested_size);
  const int saved_errno = errno;
  const long double probability =
      -std::expm1(-static_cast<long double>(effective_size) /
                  static_cast<long double>(sampler->interval));
  errno = saved_errno;
  // A request spanning multiple Poisson events is recorded once, then starts
  // a fresh countdown at the request's end.
  const Countdown countdown = DrawCountdown(sampler->interval, sampler);
  sampler->countdown_low = static_cast<uint64_t>(countdown);
  sampler->countdown_high = static_cast<uint64_t>(countdown >> 64);
  return probability;
}

inline bool SampleRequest(size_t requested_size, ThreadSampler *sampler,
                          long double *probability) {
  const uint64_t effective_size =
      requested_size == 0 ? 1 : static_cast<uint64_t>(requested_size);
  if (!ConsumeCountdownBytes(effective_size, sampler)) return false;
  *probability = FinishSelectedRequest(requested_size, sampler);
  return true;
}

uint64_t HashBytes(const char *data, size_t size) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

uint64_t HashFrames(const StoredFrame *frames, int count) {
  uint64_t hash = 1469598103934665603ULL;
  for (int i = 0; i < count; ++i) {
    const uint32_t values[] = {frames[i].name_id, frames[i].filename_id,
                               static_cast<uint32_t>(frames[i].line)};
    for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
      uint32_t value = values[j];
      for (int b = 0; b < 4; ++b) {
        hash ^= static_cast<unsigned char>(value & 0xff);
        hash *= 1099511628211ULL;
        value >>= 8;
      }
    }
  }
  return hash == 0 ? 1 : hash;
}

bool EqualFrames(const StackEntry &entry, const StoredFrame *frames,
                 int count) {
  if (entry.frame_count != count) return false;
  for (int i = 0; i < count; ++i) {
    if (entry.frames[i].name_id != frames[i].name_id ||
        entry.frames[i].filename_id != frames[i].filename_id ||
        entry.frames[i].line != frames[i].line) {
      return false;
    }
  }
  return true;
}

void AddUnknown(long double objects, long double bytes) {
  if (g_data == nullptr) return;
  g_data->unknown_objects += objects;
  g_data->unknown_bytes += bytes;
}

void AddOverflow(long double objects, long double bytes) {
  if (g_data == nullptr) return;
  g_data->overflow_objects += objects;
  g_data->overflow_bytes += bytes;
}

size_t EncodeCodePoint(uint32_t codepoint, char *output) {
  if (codepoint <= 0x7f) {
    output[0] = static_cast<char>(codepoint);
    return 1;
  }
  if (codepoint <= 0x7ff) {
    output[0] = static_cast<char>(0xc0 | (codepoint >> 6));
    output[1] = static_cast<char>(0x80 | (codepoint & 0x3f));
    return 2;
  }
  if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
    // Surrogates are not valid UTF-8 scalar values; show them as replacement
    // characters while keeping the callback independent of Python encoders.
    output[0] = static_cast<char>(0xef);
    output[1] = static_cast<char>(0xbf);
    output[2] = static_cast<char>(0xbd);
    return 3;
  }
  if (codepoint <= 0xffff) {
    output[0] = static_cast<char>(0xe0 | (codepoint >> 12));
    output[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    output[2] = static_cast<char>(0x80 | (codepoint & 0x3f));
    return 3;
  }
  output[0] = static_cast<char>(0xf0 | (codepoint >> 18));
  output[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
  output[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
  output[3] = static_cast<char>(0x80 | (codepoint & 0x3f));
  return 4;
}

bool InternEncodedBuffer(const char *buffer, size_t length, uint32_t *id) {
  const uint64_t hash = HashBytes(buffer, length);
  const uint32_t initial = static_cast<uint32_t>(hash) & (kStringHashSlots - 1);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe) {
    const uint32_t slot = (initial + probe) & (kStringHashSlots - 1);
    const uint32_t entry_plus_one = g_data->string_slots[slot];
    if (entry_plus_one != 0) {
      const uint32_t candidate = entry_plus_one - 1;
      const StringEntry &entry = g_data->strings[candidate];
      if (entry.hash == hash && entry.length == length &&
          memcmp(g_data->string_arena + entry.offset, buffer, length) == 0) {
        *id = candidate;
        return true;
      }
      continue;
    }
    if (g_data->string_count >= kMaxStrings ||
        g_data->string_bytes + length + 1 > kMaxStringBytes) {
      return false;
    }
    const uint32_t new_id = g_data->string_count++;
    StringEntry &entry = g_data->strings[new_id];
    entry.hash = hash;
    entry.offset = g_data->string_bytes;
    entry.length = static_cast<uint16_t>(length);
    memcpy(g_data->string_arena + g_data->string_bytes, buffer, length);
    g_data->string_arena[g_data->string_bytes + length] = '\0';
    g_data->string_bytes += static_cast<uint32_t>(length + 1);
    g_data->string_slots[slot] = new_id + 1;
    *id = new_id;
    return true;
  }
  return false;
}

bool InternUnicode(PyObject *unicode, const char *fallback, uint32_t *id,
                   const char *extra_marker = nullptr) {
  char bounded[kMaxStringLength];
  const size_t trunc_marker_length = sizeof(kTruncatedMarker) - 1;
  const size_t extra_marker_length =
      extra_marker == nullptr ? 0 : strlen(extra_marker);
  const size_t available = kMaxStringLength - trunc_marker_length -
                           extra_marker_length;
  size_t length = 0;
  bool truncated = false;

  if (unicode == nullptr || !PyUnicode_Check(unicode)) {
    const size_t fallback_length = strlen(fallback);
    length = std::min(fallback_length, available);
    memcpy(bounded, fallback, length);
    truncated = fallback_length > length;
  } else {
    // Code names and filenames are ready Unicode objects owned by the live
    // frame. Read their code points directly; Unicode_AsUTF8AndSize can
    // allocate and cache a second representation from inside the allocator.
    const int kind = PyUnicode_KIND(unicode);
    void *data = PyUnicode_DATA(unicode);
    const Py_ssize_t char_count = PyUnicode_GET_LENGTH(unicode);
    for (Py_ssize_t i = 0; i < char_count; ++i) {
      char encoded[4];
      const uint32_t codepoint =
          static_cast<uint32_t>(PyUnicode_READ(kind, data, i));
      const size_t encoded_length = EncodeCodePoint(codepoint, encoded);
      if (length + encoded_length > available) {
        truncated = true;
        break;
      }
      memcpy(bounded + length, encoded, encoded_length);
      length += encoded_length;
    }
  }
  if (truncated) {
    memcpy(bounded + length, kTruncatedMarker, trunc_marker_length);
    length += trunc_marker_length;
  }
  if (extra_marker_length != 0) {
    memcpy(bounded + length, extra_marker, extra_marker_length);
    length += extra_marker_length;
  }
  return InternEncodedBuffer(bounded, length, id);
}

bool FindOrAddStack(const StoredFrame *frames, int frame_count, uint64_t hash,
                    long double objects, long double bytes) {
  const uint32_t initial = static_cast<uint32_t>(hash) & (kStackHashSlots - 1);
  for (uint32_t probe = 0; probe < kMaxProbe; ++probe) {
    const uint32_t slot = (initial + probe) & (kStackHashSlots - 1);
    const uint16_t entry_plus_one = g_data->stack_slots[slot];
    if (entry_plus_one != 0) {
      StackEntry &entry = g_data->stacks[entry_plus_one - 1];
      if (entry.hash == hash && EqualFrames(entry, frames, frame_count)) {
        entry.objects += objects;
        entry.bytes += bytes;
        return true;
      }
      continue;
    }
    if (g_data->stack_count >= kMaxStacks ||
        g_data->total_export_frames + frame_count > kMaxExportFrames) {
      return false;
    }
    const uint32_t index = g_data->stack_count++;
    StackEntry &entry = g_data->stacks[index];
    entry.hash = hash;
    entry.objects = objects;
    entry.bytes = bytes;
    entry.frame_count = static_cast<uint16_t>(frame_count);
    memcpy(entry.frames, frames, sizeof(StoredFrame) * frame_count);
    g_data->total_export_frames += frame_count;
    g_data->stack_slots[slot] = static_cast<uint16_t>(index + 1);
    return true;
  }
  return false;
}

void RecordSelectedAllocation(size_t requested_size, long double probability,
                              PyThreadState *thread_state) {
  if (g_data != nullptr &&
      g_data->selected_samples != std::numeric_limits<uint64_t>::max()) {
    ++g_data->selected_samples;
  }
  if (g_data == nullptr || probability <= 0.0L ||
      !std::isfinite(static_cast<double>(probability))) {
    if (g_data != nullptr) g_data->invalid_estimate = true;
    return;
  }
  const long double objects = 1.0L / probability;
  const long double bytes = static_cast<long double>(requested_size) /
                            probability;
  if (!std::isfinite(static_cast<double>(objects)) ||
      !std::isfinite(static_cast<double>(bytes))) {
    g_data->invalid_estimate = true;
    return;
  }

  CallFrame captured[kMaxFramesToCapture];
  bool walker_truncated = false;
  const int frame_count = PopulateFrames(captured, thread_state,
                                         kMaxFramesToCapture,
                                         &walker_truncated);
  if (frame_count <= 0) {
    AddUnknown(objects, bytes);
    return;
  }

  StoredFrame frames[kMaxFramesToCapture];
  int usable_frames = 0;
  bool stack_truncated = walker_truncated ||
                         frame_count == kMaxFramesToCapture;
  for (int i = 0; i < frame_count; ++i) {
    if (captured[i].py_code == nullptr) {
      AddUnknown(objects, bytes);
      return;
    }
    PyCodeObject *code = captured[i].py_code;
    bool line_budget_exceeded = g_data->line_lookups >= kMaxLineLookups;
    int line = captured[i].lineno;
#if PY_VERSION_HEX >= 0x030B0000
    const bool valid_line_table = PyBytes_Check(code->co_linetable);
    const size_t line_table_size =
        valid_line_table ? static_cast<size_t>(PyBytes_GET_SIZE(code->co_linetable))
                         : static_cast<size_t>(kMaxLineTableBytes + 1);
    line_budget_exceeded =
        line_budget_exceeded || line_table_size > kMaxLineTableBytes ||
        line_table_size >
            kMaxLineBytesPerCollection - g_data->line_bytes_visited;
    if (line_budget_exceeded) {
      line = 0;
    } else {
      ++g_data->line_lookups;
      g_data->line_bytes_visited += static_cast<uint32_t>(line_table_size);
      line = PyCode_Addr2Line(code, captured[i].lineno);
    }
#else
    ++g_data->line_lookups;
#endif

    const char *line_marker = line_budget_exceeded ? kLineTruncatedMarker :
                              (stack_truncated && i == frame_count - 1)
                                  ? kStackTruncatedMarker
                                  : nullptr;
    uint32_t name_id = 0;
    uint32_t filename_id = 0;
    if (!InternUnicode(code->co_name, "[unknown]", &name_id, line_marker) ||
        !InternUnicode(code->co_filename, "[unknown]", &filename_id)) {
      AddOverflow(objects, bytes);
      return;
    }
    frames[usable_frames].name_id = name_id;
    frames[usable_frames].filename_id = filename_id;
    frames[usable_frames].line = line;
    ++usable_frames;
  }

  const uint64_t hash = HashFrames(frames, usable_frames);
  if (!FindOrAddStack(frames, usable_frames, hash, objects, bytes)) {
    AddOverflow(objects, bytes);
  }
}

PyThreadState *CurrentThreadStateUnchecked() {
#if PY_VERSION_HEX >= 0x030D0000
  return PyThreadState_GetUnchecked();
#else
  return _PyThreadState_UncheckedGet();
#endif
}

void ObserveSelectedAllocation(size_t requested_size, uint64_t generation,
                               long double probability) {
  // Probability math and countdown draws preserve errno themselves. Save the
  // allocator's errno only for the selected path, where interpreter checks and
  // frame attribution can call runtime or libc helpers.
  const int saved_errno = errno;
  if (g_hooks_disabled.load(std::memory_order_acquire)) {
    errno = saved_errno;
    return;
  }
  PyThreadState *thread_state = CurrentThreadStateUnchecked();
  if (thread_state == nullptr) {
    errno = saved_errno;
    return;
  }
#if PY_VERSION_HEX >= 0x03090000
  PyInterpreterState *interpreter = PyThreadState_GetInterpreter(thread_state);
#else
  PyInterpreterState *interpreter = thread_state->interp;
#endif
  if (interpreter != g_main_interpreter ||
      g_active_generation.load(std::memory_order_acquire) != generation) {
    errno = saved_errno;
    return;
  }

  PyObject *error_type = nullptr;
  PyObject *error_value = nullptr;
  PyObject *error_traceback = nullptr;
  PyErr_Fetch(&error_type, &error_value, &error_traceback);
  RecordSelectedAllocation(requested_size, probability, thread_state);
  PyErr_Clear();
  PyErr_Restore(error_type, error_value, error_traceback);
  errno = saved_errno;
}

__attribute__((noinline))
void ObserveSelectedRequest(size_t requested_size, uint64_t generation,
                            ThreadSampler *sampler) {
  const long double probability = FinishSelectedRequest(requested_size, sampler);
  ObserveSelectedAllocation(requested_size, generation, probability);
}

inline void ObserveAllocationCandidate(size_t requested_size,
                                       uint64_t generation,
                                       ThreadSampler *sampler) {
  // The shared per-thread Poisson stream can advance on allocations from
  // unsupported interpreters; selected events are rejected before collector
  // access, and restricting the stream to main-interpreter allocations keeps
  // the same Poisson distribution.
  const uint64_t effective_size =
      requested_size == 0 ? 1 : static_cast<uint64_t>(requested_size);
  if (ConsumeCountdownBytes(effective_size, sampler)) {
    ObserveSelectedRequest(requested_size, generation, sampler);
  }
}

// All MEM/OBJ operations share one outermost-request boundary. The delegate
// is a compile-time callable so these checks inline into each allocator hook.
template <typename Allocate>
inline void *ProfileAllocation(size_t size, bool valid_size,
                               const Allocate &allocate) {
#if defined(GCLOUDPROFILER_BENCH_STAGE) && GCLOUDPROFILER_BENCH_STAGE == 0
  (void)size;
  (void)valid_size;
  return allocate();
#else
  const uint64_t generation =
      g_active_generation.load(std::memory_order_acquire);
  if (generation == 0) return allocate();

#if defined(GCLOUDPROFILER_BENCH_STAGE) && GCLOUDPROFILER_BENCH_STAGE == 1
  // Keep the active branch and its load visible in generated code.
  asm volatile("" : : "r"(generation) : "memory");
  return allocate();
#else

  // Resolve TLS once and retain its address across the delegate call. Volatile
  // prevents GCC from resolving TLS again after a potentially reentrant call.
  ThreadSampler *volatile sampler = CurrentThreadSampler();
  if (sampler->in_hook) return allocate();
  sampler->in_hook = true;
  void *result = allocate();
#if !defined(GCLOUDPROFILER_BENCH_STAGE) || GCLOUDPROFILER_BENCH_STAGE == 3
  if (result != nullptr && valid_size) {
#if defined(GCLOUDPROFILER_BENCH_STAGE)
    long double probability;
    SampleRequest(size, sampler, &probability);
#else
    ObserveAllocationCandidate(size, generation, sampler);
#endif
  }
#else
  (void)size;
  (void)valid_size;
#endif
  sampler->in_hook = false;
  return result;
#endif
#endif
}

template <PyMemAllocatorEx *Allocator>
void *Malloc(void *context, size_t size) {
  return ProfileAllocation(size, true, [=]() {
    return Allocator->malloc(context, size);
  });
}

template <PyMemAllocatorEx *Allocator>
void *Calloc(void *context, size_t count, size_t size) {
  // Still delegate overflowing requests and suppress nested hooks; never
  // attribute their wrapped product as a successful allocation size.
  const bool valid_size = count == 0 || size <= SIZE_MAX / count;
  return ProfileAllocation(count * size, valid_size, [=]() {
    return Allocator->calloc(context, count, size);
  });
}

template <PyMemAllocatorEx *Allocator>
void *Realloc(void *context, void *pointer, size_t size) {
  return ProfileAllocation(size, true, [=]() {
    return Allocator->realloc(context, pointer, size);
  });
}

template <PyMemAllocatorEx *Allocator>
PyMemAllocatorEx WrappedAllocator() {
  // Allocation traffic needs no free hook. Preserve the original context so
  // free can keep calling its original function directly, including when a
  // third-party or debug allocator is underneath us.
  return {Allocator->ctx, Malloc<Allocator>, Calloc<Allocator>,
          Realloc<Allocator>, Allocator->free};
}

bool SameAllocator(const PyMemAllocatorEx &a, const PyMemAllocatorEx &b) {
  return a.ctx == b.ctx && a.malloc == b.malloc && a.calloc == b.calloc &&
         a.realloc == b.realloc && a.free == b.free;
}

void *ReplacementMalloc(void *context, size_t size) {
  PyMemAllocatorEx *underlying = static_cast<PyMemAllocatorEx *>(context);
  return underlying->malloc(underlying->ctx, size);
}

void *ReplacementCalloc(void *context, size_t count, size_t size) {
  PyMemAllocatorEx *underlying = static_cast<PyMemAllocatorEx *>(context);
  return underlying->calloc(underlying->ctx, count, size);
}

void *ReplacementRealloc(void *context, void *pointer, size_t size) {
  PyMemAllocatorEx *underlying = static_cast<PyMemAllocatorEx *>(context);
  return underlying->realloc(underlying->ctx, pointer, size);
}

void ReplacementFree(void *context, void *pointer) {
  PyMemAllocatorEx *underlying = static_cast<PyMemAllocatorEx *>(context);
  underlying->free(underlying->ctx, pointer);
}

bool IsMainInterpreter(PyThreadState *thread_state,
                       PyInterpreterState **interpreter) {
  if (thread_state == nullptr) return false;
#if PY_VERSION_HEX >= 0x03090000
  *interpreter = PyThreadState_GetInterpreter(thread_state);
#else
  *interpreter = thread_state->interp;
#endif
  return *interpreter != nullptr && *interpreter == PyInterpreterState_Main();
}

PyInterpreterState *CurrentMainInterpreter() {
  PyThreadState *thread_state = CurrentThreadStateUnchecked();
  if (thread_state == nullptr) return nullptr;
  PyInterpreterState *interpreter = nullptr;
  return IsMainInterpreter(thread_state, &interpreter) ? interpreter : nullptr;
}

bool HooksIntact() {
  PyMemAllocatorEx mem;
  PyMemAllocatorEx obj;
  PyMem_GetAllocator(PYMEM_DOMAIN_MEM, &mem);
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &obj);
  return SameAllocator(mem, WrappedAllocator<&g_mem_allocator>()) &&
         SameAllocator(obj, WrappedAllocator<&g_obj_allocator>());
}

void AfterForkChild() {
  g_active_generation.store(0, std::memory_order_release);
  g_after_fork = true;
}

bool CheckedAddUnknown(PyObject *dict, const char *name, long double objects,
                       long double bytes);

bool ToInt64(long double value, int64_t *result) {
  if (!std::isfinite(static_cast<double>(value)) || value < 0.0L ||
      value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
    return false;
  }
  const long double rounded = std::floor(value + 0.5L);
  if (rounded > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
    return false;
  }
  *result = static_cast<int64_t>(rounded);
  return true;
}

bool AddTrace(PyObject *dict, PyObject *trace, long double objects,
              long double bytes) {
  int64_t rounded_objects = 0;
  int64_t rounded_bytes = 0;
  if (!ToInt64(objects, &rounded_objects) || !ToInt64(bytes, &rounded_bytes)) {
    PyErr_SetString(PyExc_OverflowError,
                    "allocation profile estimate exceeds pprof int64 values");
    return false;
  }
  PyObject *values = PyTuple_New(2);
  if (values == nullptr) return false;
  PyObject *object_value = PyLong_FromLongLong(rounded_objects);
  PyObject *byte_value = PyLong_FromLongLong(rounded_bytes);
  if (object_value == nullptr || byte_value == nullptr) {
    Py_XDECREF(object_value);
    Py_XDECREF(byte_value);
    Py_DECREF(values);
    return false;
  }
  PyTuple_SET_ITEM(values, 0, object_value);
  PyTuple_SET_ITEM(values, 1, byte_value);
  const int status = PyDict_SetItem(dict, trace, values);
  Py_DECREF(values);
  return status == 0;
}

bool CheckedAddUnknown(PyObject *dict, const char *name, long double objects,
                       long double bytes) {
  if (objects == 0.0L && bytes == 0.0L) return true;
  PyObject *function = PyUnicode_FromString(name);
  PyObject *filename = PyUnicode_FromString("");
  PyObject *frame = PyTuple_New(3);
  PyObject *trace = PyTuple_New(1);
  if (function == nullptr || filename == nullptr || frame == nullptr ||
      trace == nullptr) {
    Py_XDECREF(function);
    Py_XDECREF(filename);
    Py_XDECREF(frame);
    Py_XDECREF(trace);
    return false;
  }
  PyObject *line = PyLong_FromLong(0);
  if (line == nullptr) {
    Py_DECREF(frame);
    Py_DECREF(trace);
    Py_DECREF(function);
    Py_DECREF(filename);
    return false;
  }
  PyTuple_SET_ITEM(frame, 0, function);
  PyTuple_SET_ITEM(frame, 1, filename);
  PyTuple_SET_ITEM(frame, 2, line);
  PyTuple_SET_ITEM(trace, 0, frame);
  const bool result = AddTrace(dict, trace, objects, bytes);
  Py_DECREF(trace);
  return result;
}

PyObject *ExportTraces() {
  PyObject *dict = PyDict_New();
  if (dict == nullptr) return nullptr;
  PyObject **py_strings = static_cast<PyObject **>(
      calloc(g_data->string_count == 0 ? 1 : g_data->string_count,
             sizeof(PyObject *)));
  if (py_strings == nullptr) {
    Py_DECREF(dict);
    return PyErr_NoMemory();
  }
  for (uint32_t i = 0; i < g_data->string_count; ++i) {
    const StringEntry &entry = g_data->strings[i];
    py_strings[i] = PyUnicode_DecodeUTF8(
        g_data->string_arena + entry.offset, entry.length, "replace");
    if (py_strings[i] == nullptr) {
      for (uint32_t j = 0; j < i; ++j) Py_DECREF(py_strings[j]);
      free(py_strings);
      Py_DECREF(dict);
      return nullptr;
    }
  }

  for (uint32_t i = 0; i < g_data->stack_count; ++i) {
    const StackEntry &entry = g_data->stacks[i];
    PyObject *trace = PyTuple_New(entry.frame_count);
    if (trace == nullptr) goto error;
    for (uint16_t j = 0; j < entry.frame_count; ++j) {
      const StoredFrame &stored = entry.frames[j];
      PyObject *frame = PyTuple_New(3);
      if (frame == nullptr) {
        Py_DECREF(trace);
        goto error;
      }
      Py_INCREF(py_strings[stored.name_id]);
      PyTuple_SET_ITEM(frame, 0, py_strings[stored.name_id]);
      Py_INCREF(py_strings[stored.filename_id]);
      PyTuple_SET_ITEM(frame, 1, py_strings[stored.filename_id]);
      PyObject *line = PyLong_FromLong(stored.line);
      if (line == nullptr) {
        Py_DECREF(frame);
        Py_DECREF(trace);
        goto error;
      }
      PyTuple_SET_ITEM(frame, 2, line);
      PyTuple_SET_ITEM(trace, j, frame);
    }
    if (!AddTrace(dict, trace, entry.objects, entry.bytes)) {
      Py_DECREF(trace);
      goto error;
    }
    Py_DECREF(trace);
  }
  if (!CheckedAddUnknown(dict, kUnknownName, g_data->unknown_objects,
                         g_data->unknown_bytes) ||
      !CheckedAddUnknown(dict, kOverflowName, g_data->overflow_objects,
                         g_data->overflow_bytes)) {
    goto error;
  }
  for (uint32_t i = 0; i < g_data->string_count; ++i) Py_DECREF(py_strings[i]);
  free(py_strings);
  return dict;

error:
  for (uint32_t i = 0; i < g_data->string_count; ++i) Py_XDECREF(py_strings[i]);
  free(py_strings);
  Py_DECREF(dict);
  return nullptr;
}

bool SetDiagnosticCount(PyObject *dict, const char *key, uint64_t value) {
  PyObject *number = PyLong_FromUnsignedLongLong(
      static_cast<unsigned long long>(value));
  if (number == nullptr) return false;
  const int status = PyDict_SetItemString(dict, key, number);
  Py_DECREF(number);
  return status == 0;
}

bool SetDiagnosticEstimate(PyObject *dict, const char *key,
                           long double value) {
  const double converted = static_cast<double>(value);
  if (!std::isfinite(converted)) {
    PyErr_SetString(PyExc_OverflowError,
                    "allocation diagnostics contain a non-finite estimate");
    return false;
  }
  PyObject *number = PyFloat_FromDouble(converted);
  if (number == nullptr) return false;
  const int status = PyDict_SetItemString(dict, key, number);
  Py_DECREF(number);
  return status == 0;
}

PyObject *ExportDiagnostics(int64_t duration_ns, int64_t export_duration_ns) {
  long double attributed_objects = 0.0L;
  long double attributed_bytes = 0.0L;
  for (uint32_t i = 0; i < g_data->stack_count; ++i) {
    attributed_objects += g_data->stacks[i].objects;
    attributed_bytes += g_data->stacks[i].bytes;
  }

  PyObject *diagnostics = PyDict_New();
  if (diagnostics == nullptr) return nullptr;
  if (!SetDiagnosticCount(diagnostics, "selected_samples",
                          g_data->selected_samples) ||
      !SetDiagnosticEstimate(diagnostics, "attributed_objects",
                             attributed_objects) ||
      !SetDiagnosticEstimate(diagnostics, "attributed_bytes",
                             attributed_bytes) ||
      !SetDiagnosticEstimate(diagnostics, "unknown_objects",
                             g_data->unknown_objects) ||
      !SetDiagnosticEstimate(diagnostics, "unknown_bytes",
                             g_data->unknown_bytes) ||
      !SetDiagnosticEstimate(diagnostics, "overflow_objects",
                             g_data->overflow_objects) ||
      !SetDiagnosticEstimate(diagnostics, "overflow_bytes",
                             g_data->overflow_bytes) ||
      !SetDiagnosticCount(diagnostics, "storage_bytes", sizeof(*g_data)) ||
      !SetDiagnosticCount(diagnostics, "stack_count", g_data->stack_count) ||
      !SetDiagnosticCount(diagnostics, "stack_capacity", kMaxStacks) ||
      !SetDiagnosticCount(diagnostics, "export_frames",
                          g_data->total_export_frames) ||
      !SetDiagnosticCount(diagnostics, "export_frame_capacity",
                          kMaxExportFrames) ||
      !SetDiagnosticCount(diagnostics, "string_count", g_data->string_count) ||
      !SetDiagnosticCount(diagnostics, "string_capacity", kMaxStrings) ||
      !SetDiagnosticCount(diagnostics, "string_bytes", g_data->string_bytes) ||
      !SetDiagnosticCount(diagnostics, "string_bytes_capacity",
                          kMaxStringBytes) ||
      !SetDiagnosticCount(diagnostics, "line_lookups", g_data->line_lookups) ||
      !SetDiagnosticCount(diagnostics, "line_bytes_visited",
                          g_data->line_bytes_visited) ||
      !SetDiagnosticCount(diagnostics, "collection_duration_ns",
                          static_cast<uint64_t>(duration_ns)) ||
      !SetDiagnosticCount(diagnostics, "export_duration_ns",
                          static_cast<uint64_t>(export_duration_ns))) {
    Py_DECREF(diagnostics);
    return nullptr;
  }
  return diagnostics;
}

}  // namespace

bool InitializeMemoryProfiler(uint64_t sampling_interval_bytes) {
#if defined(Py_GIL_DISABLED)
  // The collector and its fixed hash tables are intentionally protected by the
  // GIL. A free-threaded interpreter may report attached thread states without
  // holding a process-wide GIL, so allocator callbacks cannot safely collect.
  (void)sampling_interval_bytes;
  return false;
#else
  if (sampling_interval_bytes == 0 ||
      sampling_interval_bytes >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return false;
  }
  PyInterpreterState *interpreter = CurrentMainInterpreter();
  if (interpreter == nullptr ||
      g_hooks_disabled.load(std::memory_order_acquire)) {
    return false;
  }

  if (g_hooks_installed) {
    if (!HooksIntact()) {
      g_hooks_disabled.store(true, std::memory_order_release);
      return false;
    }
    if (sampling_interval_bytes != g_sampling_interval) return false;
    if (g_after_fork) {
      g_main_interpreter = interpreter;
      g_after_fork = false;
      memset(g_data, 0, sizeof(*g_data));
    }
    return g_main_interpreter == interpreter;
  }

  CollectorData *data = static_cast<CollectorData *>(calloc(1, sizeof(*data)));
  if (data == nullptr) return false;
  g_data = data;
  g_sampling_interval = sampling_interval_bytes;
  g_main_interpreter = interpreter;

  PyMem_GetAllocator(PYMEM_DOMAIN_MEM, &g_mem_allocator);
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &g_obj_allocator);
  PyMemAllocatorEx mem = WrappedAllocator<&g_mem_allocator>();
  PyMemAllocatorEx obj = WrappedAllocator<&g_obj_allocator>();
  PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &mem);
  PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &obj);
  if (!HooksIntact()) {
    g_hooks_disabled.store(true, std::memory_order_release);
    return false;
  }
  g_hooks_installed = true;
  if (pthread_atfork(nullptr, nullptr, AfterForkChild) != 0) {
    // The wrappers are process-lifetime once installed. Leave them as
    // pass-through hooks, but do not start collections without fork fencing.
    g_hooks_disabled.store(true, std::memory_order_release);
    return false;
  }
  return true;
#endif
}

bool MemoryProfilerAvailable() {
  PyInterpreterState *interpreter = CurrentMainInterpreter();
  if (interpreter == nullptr || interpreter != g_main_interpreter) {
    return false;
  }
  if (!g_hooks_installed ||
      g_hooks_disabled.load(std::memory_order_acquire) || g_data == nullptr ||
      g_after_fork) {
    return false;
  }
  if (!HooksIntact()) {
    g_hooks_disabled.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

size_t MemoryProfilerStorageBytes() {
  return g_data == nullptr ? 0 : sizeof(*g_data);
}

bool StartMemoryProfile() {
  PyInterpreterState *interpreter = CurrentMainInterpreter();
  if (interpreter == nullptr || interpreter != g_main_interpreter) {
    return false;
  }
  if (!MemoryProfilerAvailable()) return false;
  // Start is owned by the main interpreter and the GIL serializes callers.
  // Check before clearing storage so an overlapping request cannot disturb the
  // active collection.
  if (g_active_generation.load(std::memory_order_acquire) != 0) return false;
  memset(g_data, 0, sizeof(*g_data));
  uint64_t generation = ++g_next_generation;
  if (generation == 0) generation = ++g_next_generation;
  timespec start_wall;
  if (clock_gettime(CLOCK_REALTIME, &start_wall) != 0 ||
      clock_gettime(CLOCK_MONOTONIC, &g_collection_start_monotonic) != 0) {
    return false;
  }
  g_collection_start_wall_ns =
      static_cast<int64_t>(start_wall.tv_sec) * 1000000000LL +
      static_cast<int64_t>(start_wall.tv_nsec);
  g_active_generation.store(generation, std::memory_order_release);
  return true;
}

PyObject *StopMemoryProfile() {
  PyInterpreterState *interpreter = CurrentMainInterpreter();
  if (interpreter == nullptr || interpreter != g_main_interpreter) {
    PyErr_SetString(PyExc_RuntimeError,
                    "memory profile collection is owned by the main "
                    "interpreter");
    return nullptr;
  }
  if (g_active_generation.exchange(0, std::memory_order_acq_rel) == 0) {
    PyErr_SetString(PyExc_RuntimeError,
                    "memory profiling collection is not active");
    return nullptr;
  }
  timespec stop_monotonic;
  if (clock_gettime(CLOCK_MONOTONIC, &stop_monotonic) != 0) {
    PyErr_SetFromErrno(PyExc_OSError);
    return nullptr;
  }
  const int64_t duration_ns =
      (static_cast<int64_t>(stop_monotonic.tv_sec) -
       static_cast<int64_t>(g_collection_start_monotonic.tv_sec)) *
          1000000000LL +
      (static_cast<int64_t>(stop_monotonic.tv_nsec) -
       static_cast<int64_t>(g_collection_start_monotonic.tv_nsec));
  if (g_data->invalid_estimate) {
    PyErr_SetString(PyExc_OverflowError,
                    "allocation profile contains an invalid estimate");
    return nullptr;
  }
  if (!HooksIntact()) {
    g_hooks_disabled.store(true, std::memory_order_release);
    PyErr_SetString(PyExc_RuntimeError,
                    "memory allocator hooks were replaced during collection");
    return nullptr;
  }
  timespec export_start;
  timespec export_stop;
  if (clock_gettime(CLOCK_MONOTONIC, &export_start) != 0) {
    PyErr_SetFromErrno(PyExc_OSError);
    return nullptr;
  }
  PyObject *traces = ExportTraces();
  if (traces == nullptr) return nullptr;
  if (clock_gettime(CLOCK_MONOTONIC, &export_stop) != 0) {
    Py_DECREF(traces);
    PyErr_SetFromErrno(PyExc_OSError);
    return nullptr;
  }
  const int64_t export_duration_ns =
      (static_cast<int64_t>(export_stop.tv_sec) -
       static_cast<int64_t>(export_start.tv_sec)) *
          1000000000LL +
      (static_cast<int64_t>(export_stop.tv_nsec) -
       static_cast<int64_t>(export_start.tv_nsec));
  PyObject *diagnostics = ExportDiagnostics(duration_ns, export_duration_ns);
  if (diagnostics == nullptr) {
    Py_DECREF(traces);
    return nullptr;
  }
  return Py_BuildValue("(NLLN)", traces,
                       static_cast<long long>(duration_ns),
                       static_cast<long long>(g_collection_start_wall_ns),
                       diagnostics);
}

bool TestMemorySampling(size_t requested_size, uint64_t interval,
                        double countdown, bool *selected,
                        double *probability, double *objects,
                        double *bytes) {
  if (interval == 0 || !std::isfinite(countdown) || countdown < 0.0 ||
      countdown > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
    return false;
  }
  ThreadSampler sampler = {};
  sampler.interval = interval;
  sampler.rng = 1;
  // A zero test countdown means force selection; production uses zero only
  // as the uninitialized sentinel and gives zero-byte requests a size of one.
  const Countdown initial = std::max<Countdown>(
      1, static_cast<Countdown>(std::ceil(countdown)));
  sampler.countdown_low = static_cast<uint64_t>(initial);
  sampler.countdown_high = static_cast<uint64_t>(initial >> 64);
  long double inclusion_probability = 0;
  *selected = SampleRequest(requested_size, &sampler, &inclusion_probability);
  *probability = static_cast<double>(inclusion_probability);
  *objects = *selected ? static_cast<double>(1.0L / inclusion_probability) : 0;
  *bytes = *selected
               ? static_cast<double>(requested_size / inclusion_probability)
               : 0;
  return true;
}

bool TestMemoryCallbackPreservation(bool *exception_preserved,
                                   bool *errno_preserved) {
  if (g_active_generation.load(std::memory_order_acquire) == 0 ||
      !HooksIntact() ||
      CurrentMainInterpreter() != g_main_interpreter) {
    return false;
  }
  errno = EDOM;
  void *baseline = g_mem_allocator.malloc(g_mem_allocator.ctx, 64);
  const int expected_errno = errno;
  if (baseline != nullptr) g_mem_allocator.free(g_mem_allocator.ctx, baseline);

  PyErr_SetString(PyExc_RuntimeError, "memory callback preservation test");
  errno = EDOM;
  void *result = PyMem_Malloc(64);
  const int observed_errno = errno;
  *exception_preserved = PyErr_ExceptionMatches(PyExc_RuntimeError) != 0;
  PyErr_Clear();
  if (result != nullptr) PyMem_Free(result);
  *errno_preserved = observed_errno == expected_errno;
  return result != nullptr;
}

bool TestMemoryFastPathPreservation(bool *exception_preserved,
                                    bool *errno_preserved,
                                    bool *sample_count_unchanged) {
  const uint64_t generation =
      g_active_generation.load(std::memory_order_acquire);
  if (generation == 0 || !HooksIntact() ||
      CurrentMainInterpreter() != g_main_interpreter) {
    return false;
  }

  ThreadSampler *sampler = CurrentThreadSampler();
  const ThreadSampler previous = *sampler;
  sampler->countdown_low = std::numeric_limits<uint64_t>::max();
  sampler->countdown_high = std::numeric_limits<uint64_t>::max();
  const uint64_t samples_before = g_data->selected_samples;

  errno = EDOM;
  void *baseline = g_mem_allocator.malloc(g_mem_allocator.ctx, 64);
  const int expected_errno = errno;
  if (baseline == nullptr) {
    *sampler = previous;
    return false;
  }
  g_mem_allocator.free(g_mem_allocator.ctx, baseline);

  PyErr_SetString(PyExc_RuntimeError, "unsampled callback preservation test");
  errno = EDOM;
  void *result = PyMem_Malloc(64);
  const int observed_errno = errno;
  *exception_preserved =
      PyErr_ExceptionMatches(PyExc_RuntimeError) != 0;
  PyErr_Clear();
  if (result != nullptr) PyMem_Free(result);

  *errno_preserved = observed_errno == expected_errno;
  *sample_count_unchanged = g_data->selected_samples == samples_before;
  *sampler = previous;
  return result != nullptr;
}

bool TestMemoryCountdownArithmetic() {
  ThreadSampler sampler = {};
  sampler.countdown_low = 100;
  if (ConsumeCountdownBytes(32, &sampler) ||
      sampler.countdown_low != 68 || sampler.countdown_high != 0) {
    return false;
  }

  sampler.countdown_low = 100;
  sampler.countdown_high = 1;
  if (ConsumeCountdownBytes(32, &sampler) ||
      sampler.countdown_low != 68 || sampler.countdown_high != 1) {
    return false;
  }

  sampler.countdown_low = 8;
  sampler.countdown_high = 1;
  if (ConsumeCountdownBytes(64, &sampler) ||
      sampler.countdown_low != static_cast<uint64_t>(0) - 56 ||
      sampler.countdown_high != 0) {
    return false;
  }

  sampler.countdown_low = 64;
  sampler.countdown_high = 0;
  return ConsumeCountdownBytes(64, &sampler) &&
         sampler.countdown_low == 64 && sampler.countdown_high == 0;
}

bool TestMemoryNestedHook() {
  if (g_active_generation.load(std::memory_order_acquire) == 0 ||
      g_data == nullptr ||
      !MemoryProfilerAvailable()) {
    return false;
  }
  ThreadSampler *sampler = CurrentThreadSampler();
  long double objects_before = g_data->unknown_objects +
                               g_data->overflow_objects;
  long double bytes_before = g_data->unknown_bytes + g_data->overflow_bytes;
  for (uint32_t i = 0; i < g_data->stack_count; ++i) {
    objects_before += g_data->stacks[i].objects;
    bytes_before += g_data->stacks[i].bytes;
  }
  const bool old_guard = sampler->in_hook;
  sampler->in_hook = true;
  void *allocation = PyMem_Malloc(32);
  sampler->in_hook = old_guard;
  if (allocation == nullptr) return false;
  PyMem_Free(allocation);

  long double objects_after = g_data->unknown_objects +
                              g_data->overflow_objects;
  long double bytes_after = g_data->unknown_bytes + g_data->overflow_bytes;
  for (uint32_t i = 0; i < g_data->stack_count; ++i) {
    objects_after += g_data->stacks[i].objects;
    bytes_after += g_data->stacks[i].bytes;
  }
  return objects_before == objects_after && bytes_before == bytes_after;
}

static bool RunSamplingSequence(const size_t *sizes, size_t size_count,
                                uint64_t interval, uint64_t requests,
                                uint64_t seed, uint64_t *selected,
                                double *objects, double *bytes) {
  if (interval == 0 || requests == 0 || size_count == 0) return false;
  ThreadSampler state = {};
  ThreadSampler *sampler = &state;
  sampler->rng = seed == 0 ? 1 : seed;
  sampler->interval = interval;
  const Countdown countdown = DrawCountdown(interval, sampler);
  sampler->countdown_low = static_cast<uint64_t>(countdown);
  sampler->countdown_high = static_cast<uint64_t>(countdown >> 64);
  sampler->in_hook = true;
  long double object_total = 0.0L;
  long double byte_total = 0.0L;
  uint64_t selected_total = 0;
  for (uint64_t i = 0; i < requests; ++i) {
    const size_t requested_size = sizes[i % size_count];
    long double probability;
    if (SampleRequest(requested_size, sampler, &probability)) {
      ++selected_total;
      object_total += 1.0L / probability;
      byte_total += static_cast<long double>(requested_size) /
                    probability;
    }
  }
  *selected = selected_total;
  *objects = static_cast<double>(object_total);
  *bytes = static_cast<double>(byte_total);
  return true;
}

bool RunMemorySamplingSequence(size_t requested_size, uint64_t interval,
                               uint64_t requests, uint64_t seed,
                               uint64_t *selected, double *objects,
                               double *bytes) {
  return RunSamplingSequence(&requested_size, 1, interval, requests, seed,
                             selected, objects, bytes);
}

bool RunMemoryMixedSamplingSequence(uint64_t interval, uint64_t requests,
                                   uint64_t seed, uint64_t *selected,
                                   double *objects, double *bytes) {
  const size_t sizes[] = {32, 256, 4096};
  return RunSamplingSequence(sizes, 3, interval, requests, seed, selected,
                             objects, bytes);
}

bool TestReplaceMemoryAllocator() {
  if (g_active_generation.load(std::memory_order_acquire) == 0 ||
      !HooksIntact()) {
    return false;
  }
  PyMem_GetAllocator(PYMEM_DOMAIN_MEM, &g_replacement_underlying_allocator);
  g_replacement_allocator.ctx = &g_replacement_underlying_allocator;
  g_replacement_allocator.malloc = ReplacementMalloc;
  g_replacement_allocator.calloc = ReplacementCalloc;
  g_replacement_allocator.realloc = ReplacementRealloc;
  g_replacement_allocator.free = ReplacementFree;
  PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &g_replacement_allocator);
  return true;
}
