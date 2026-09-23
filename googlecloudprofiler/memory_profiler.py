# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Sampled allocation profiler backed by the native CPython allocator hooks."""

import logging
import time

from googlecloudprofiler import builder

logger = logging.getLogger(__name__)

try:
  from googlecloudprofiler import _profiler
except ImportError:
  _profiler = None


class MemoryProfiler:
  """Collects estimated allocation traffic for one requested interval.

  This profiles successful requests through CPython's MEM and OBJ allocation
  domains. The reported values are weighted estimates of allocation traffic,
  including objects freed before collection ends. It does not report the live
  heap size.
  """

  def __init__(self, sampling_interval_bytes=524288):
    if (not isinstance(sampling_interval_bytes, int) or
        isinstance(sampling_interval_bytes, bool) or
        sampling_interval_bytes <= 0 or
        sampling_interval_bytes > (1 << 63) - 1):
      raise ValueError(
          'sampling_interval_bytes must fit a positive pprof int64')
    self._sampling_interval_bytes = sampling_interval_bytes
    self._available = bool(
        _profiler is not None and
        _profiler._memory_initialize(sampling_interval_bytes))

  @property
  def available(self):
    """Whether the native allocator hooks can still collect profiles."""
    if not self._available or _profiler is None:
      return False
    self._available = bool(_profiler._memory_available())
    return self._available

  def profile(self, duration_ns):
    """Collects and gzip-encodes a HEAP_ALLOC pprof profile.

    Args:
      duration_ns: Requested profile duration in nanoseconds.

    Returns:
      A bytes object containing gzip-compressed profile proto.
    """
    if (not isinstance(duration_ns, int) or isinstance(duration_ns, bool) or
        duration_ns < 0 or duration_ns > (1 << 63) - 1):
      raise ValueError('duration_ns must fit a nonnegative pprof int64')
    if not self.available:
      raise RuntimeError('native allocation profiling is unavailable')
    if not _profiler._memory_start():
      self._available = bool(_profiler._memory_available())
      raise RuntimeError('could not start allocation profile collection')

    try:
      if duration_ns:
        time.sleep(float(duration_ns) / 1000000000.0)
    except BaseException:
      try:
        _profiler._memory_stop()
      except BaseException:
        self._available = bool(_profiler._memory_available())
      raise

    try:
      traces, actual_duration_ns, start_time_ns, diagnostics = (
          _profiler._memory_stop())
    except BaseException:
      self._available = bool(_profiler._memory_available())
      raise

    profile_builder = builder.Builder()
    profile_builder.populate_memory_profile(
        traces, self._sampling_interval_bytes, actual_duration_ns,
        start_time_ns, diagnostics)
    logger.debug(
        'Allocation profile: selected=%d attributed=%.1f/%.1f '
        'unknown=%.1f/%.1f overflow=%.1f/%.1f storage=%d bytes '
        'stacks=%d/%d frames=%d/%d strings=%d/%d (%d bytes) '
        'export=%dns duration=%dns',
        diagnostics['selected_samples'], diagnostics['attributed_objects'],
        diagnostics['attributed_bytes'], diagnostics['unknown_objects'],
        diagnostics['unknown_bytes'], diagnostics['overflow_objects'],
        diagnostics['overflow_bytes'], diagnostics['storage_bytes'],
        diagnostics['stack_count'], diagnostics['stack_capacity'],
        diagnostics['export_frames'], diagnostics['export_frame_capacity'],
        diagnostics['string_count'], diagnostics['string_capacity'],
        diagnostics['string_bytes'],
        diagnostics['export_duration_ns'], diagnostics['collection_duration_ns'])
    return profile_builder.emit()
