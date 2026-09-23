#!/usr/bin/env python3
"""Compare allocation churn with memory hooks disabled, idle, and active.

Run from the repository root with `PYTHONPATH=.` and one mode argument:
`baseline`, `idle`, or `active`. Active mode initializes and stops a native
profile around the measured workloads. It prints throughput and p99 batch time
as JSON; this is a local microbenchmark, not a production qualification.
"""

import argparse
import json
import math
import os
import threading
import time

from googlecloudprofiler import _profiler


def _small_allocation(index):
  value = bytearray(32)
  if value:
    value[0] = index & 255


def _varied_allocation(index):
  if index % 3 == 0:
    value = bytearray(32)
  elif index % 3 == 1:
    value = bytearray(256)
  else:
    value = bytearray(4096)
  if value:
    value[0] = index & 255


def _deep_allocation(depth, index):
  if depth:
    return _deep_allocation(depth - 1, index)
  value = bytearray(128)
  value[0] = index & 255


def _operation(kind, index):
  if kind == 'small':
    _small_allocation(index)
  elif kind == 'varied':
    _varied_allocation(index)
  elif kind in ('deep', 'repeated'):
    _deep_allocation(32 if kind == 'deep' else 8, index)
  else:
    value = bytearray(128)
    value[0] = index & 255


def _percentile_99(values):
  ordered = sorted(values)
  index = min(len(ordered) - 1, math.ceil(0.99 * len(ordered)) - 1)
  return ordered[index] / 1e6


def _run_case(kind, batches, operations_per_batch):
  latencies = []
  started = time.perf_counter()
  for batch in range(batches):
    batch_started = time.perf_counter_ns()
    for index in range(operations_per_batch):
      _operation(kind, batch * operations_per_batch + index)
    latencies.append(time.perf_counter_ns() - batch_started)
  elapsed = time.perf_counter() - started
  return {
      'seconds': elapsed,
      'ops_per_second': batches * operations_per_batch / elapsed,
      'batch_p99_ms': _percentile_99(latencies),
  }


def _run_threads(batches, operations_per_batch, count):
  latencies = []

  def worker():
    worker_latencies = []
    for batch in range(batches):
      batch_started = time.perf_counter_ns()
      for index in range(operations_per_batch):
        value = bytearray(128)
        value[0] = index & 255
      worker_latencies.append(time.perf_counter_ns() - batch_started)
    latencies.extend(worker_latencies)

  started = time.perf_counter()
  workers = [threading.Thread(target=worker) for _ in range(count)]
  for thread in workers:
    thread.start()
  for thread in workers:
    thread.join()
  elapsed = time.perf_counter() - started
  return {
      'seconds': elapsed,
      'ops_per_second': count * batches * operations_per_batch / elapsed,
      'batch_p99_ms': _percentile_99(latencies),
  }


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('mode', choices=('baseline', 'idle', 'active'))
  parser.add_argument('--interval-bytes', type=int, default=4194304)
  parser.add_argument('--batches', type=int, default=200)
  parser.add_argument('--operations-per-batch', type=int, default=1000)
  parser.add_argument('--case',
                      choices=('all', 'small', 'varied', 'deep', 'repeated',
                               'threads1', 'threads'),
                      default='all')
  args = parser.parse_args()
  if min(args.interval_bytes, args.batches, args.operations_per_batch) <= 0:
    parser.error('interval and workload sizes must be positive')

  if args.mode != 'baseline':
    if not _profiler._memory_initialize(args.interval_bytes):
      raise RuntimeError('native allocation profiling is unavailable')

  if args.case == 'all':
    kinds = ('small', 'varied', 'deep', 'repeated')
  elif args.case in ('threads', 'threads1'):
    kinds = ()
  else:
    kinds = (args.case,)
  # Warm the Python call paths before timing them.
  for kind in kinds:
    for index in range(3000):
      _operation(kind, index)
  for index in range(3000):
    _operation('threads', index)

  if args.mode == 'active' and not _profiler._memory_start():
    raise RuntimeError('could not start native allocation collection')

  results = {
      kind: _run_case(kind, args.batches, args.operations_per_batch)
      for kind in kinds
  }
  results['extension_path'] = os.path.realpath(_profiler.__file__)
  results['diagnostic_stage'] = getattr(
      _profiler, '_memory_diagnostic_stage', lambda: 4)()
  if args.case in ('all', 'threads1'):
    results['threads_1x'] = _run_threads(args.batches,
                                         args.operations_per_batch, 1)
  if args.case in ('all', 'threads'):
    results['threads_4x'] = _run_threads(args.batches,
                                         args.operations_per_batch, 4)
  if args.mode == 'active':
    traces, unused_duration, unused_start, diagnostics = (
        _profiler._memory_stop())
    results['estimated_objects'] = sum(values[0] for values in traces.values())
    results['trace_count'] = len(traces)
    results['selected_samples'] = diagnostics['selected_samples']
    results['attributed_objects'] = diagnostics['attributed_objects']
    results['unknown_objects'] = diagnostics['unknown_objects']
    results['overflow_objects'] = diagnostics['overflow_objects']
    results['export_duration_ns'] = diagnostics['export_duration_ns']
  print(json.dumps(results, sort_keys=True))


if __name__ == '__main__':
  main()
