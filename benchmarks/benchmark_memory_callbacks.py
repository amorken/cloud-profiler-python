#!/usr/bin/env python3
"""Measure native MEM/OBJ callback costs in a built profiler extension."""

import argparse
import json
import statistics

from googlecloudprofiler import _profiler


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--domain', choices=('MEM', 'OBJ'), required=True)
  parser.add_argument('--operation', choices=('malloc', 'calloc', 'realloc'),
                      required=True)
  parser.add_argument('--mode', choices=('baseline', 'idle', 'active'),
                      required=True)
  parser.add_argument('--size', type=int, default=128)
  parser.add_argument('--count', type=int, default=1000000)
  parser.add_argument('--repeats', type=int, default=5)
  parser.add_argument('--interval-bytes', type=int, default=4 * 1024 * 1024)
  args = parser.parse_args()
  if min(args.size, args.count, args.repeats, args.interval_bytes) <= 0:
    parser.error('sizes and counts must be positive')
  domain = ('MEM', 'OBJ').index(args.domain) + 1
  operation = ('malloc', 'calloc', 'realloc').index(args.operation)
  stage = getattr(_profiler, '_memory_diagnostic_stage', lambda: 4)()
  if args.mode != 'baseline':
    if not _profiler._memory_initialize(args.interval_bytes):
      raise RuntimeError('allocation hooks unavailable')
  _profiler._memory_test_native_churn(domain, operation, 1000, args.size)
  if args.mode == 'active' and not _profiler._memory_start():
    raise RuntimeError('allocation collection unavailable')
  elapsed = [_profiler._memory_test_native_churn(
      domain, operation, args.count, args.size)
             for _ in range(args.repeats)]
  diagnostics = None
  if args.mode == 'active':
    unused_traces, unused_duration, unused_start, diagnostics = (
        _profiler._memory_stop())
  print(json.dumps({
      'domain': args.domain,
      'operation': args.operation,
      'mode': args.mode,
      'stage': stage,
      'size': args.size,
      'count': args.count,
      'repeats': args.repeats,
      'ns_per_request': [value / args.count for value in elapsed],
      'median_ns_per_request': statistics.median(elapsed) / args.count,
      'selected_samples': (diagnostics or {}).get('selected_samples', 0),
      'attributed_objects': (diagnostics or {}).get('attributed_objects', 0),
  }, sort_keys=True))


if __name__ == '__main__':
  main()
