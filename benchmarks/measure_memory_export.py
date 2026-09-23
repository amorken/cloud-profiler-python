#!/usr/bin/env python3
"""Measure Python peak allocation while exporting a full native profile.

Run from the repository root after building the extension. Tracemalloc starts
before the allocator hooks so the hook chain remains intact during collection.
"""

import json
import tracemalloc

from googlecloudprofiler import _profiler
from googlecloudprofiler import builder


def main():
  # Start tracking before installation so tracing becomes the delegated
  # allocator, rather than replacing the profiler hooks later.
  tracemalloc.start()
  namespace = {}
  functions = []
  for index in range(2200):
    name = 'export_stress_%04d' % index
    exec(compile('def %s():\n  return bytearray(1)\n' % name,
                 'export_stress.py', 'exec'), namespace)
    functions.append(namespace[name])

  if not _profiler._memory_initialize(1):
    raise RuntimeError('native allocation profiling is unavailable')
  if not _profiler._memory_start():
    raise RuntimeError('could not start native allocation collection')
  for function in functions:
    function()

  baseline_bytes, unused_peak = tracemalloc.get_traced_memory()
  tracemalloc.reset_peak()
  traces, duration_ns, start_time_ns, diagnostics = _profiler._memory_stop()
  profile_builder = builder.Builder()
  profile_builder.populate_memory_profile(
      traces, 1, duration_ns, start_time_ns, diagnostics)
  compressed_profile = profile_builder.emit()
  current_bytes, peak_bytes = tracemalloc.get_traced_memory()

  print(json.dumps({
      'stack_count': diagnostics['stack_count'],
      'stack_capacity': diagnostics['stack_capacity'],
      'export_frames': diagnostics['export_frames'],
      'export_frame_capacity': diagnostics['export_frame_capacity'],
      'native_storage_bytes': diagnostics['storage_bytes'],
      'python_export_peak_delta_bytes': max(0, peak_bytes - baseline_bytes),
      'python_export_current_bytes': current_bytes,
      'compressed_profile_bytes': len(compressed_profile),
      'export_duration_ns': diagnostics['export_duration_ns'],
  }, sort_keys=True))


if __name__ == '__main__':
  main()
