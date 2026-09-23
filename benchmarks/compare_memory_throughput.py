#!/usr/bin/env python3
"""Run paired, randomized allocation-profiler throughput comparisons.

Build the extension in place, then run this script with the same Python used
for the build. Each repeat measures baseline, installed-idle, and active
collection configurations in randomized order. Active losses are reported both
against the same repeat's baseline and as median throughput.
"""

import argparse
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      '--intervals',
      default='524288,1048576,2097152,4194304,8388608,16777216',
      help='comma-separated active sampling intervals in bytes')
  parser.add_argument('--repeats', type=int, default=5)
  parser.add_argument('--batches', type=int, default=3000)
  parser.add_argument('--operations-per-batch', type=int, default=1000)
  parser.add_argument(
      '--case', choices=('all', 'small', 'varied', 'deep', 'threads'),
      default='all')
  parser.add_argument('--seed', type=int, default=20260923)
  parser.add_argument(
      '--cpu', type=int,
      help='pin this process and its benchmark children to one allowed CPU')
  args = parser.parse_args()
  try:
    intervals = [int(value) for value in args.intervals.split(',')]
  except ValueError:
    parser.error('--intervals must contain comma-separated integers')
  if (args.repeats <= 0 or args.batches <= 0 or
      args.operations_per_batch <= 0 or not intervals or
      any(interval <= 0 or interval > (1 << 63) - 1
          for interval in intervals)):
    parser.error('workload sizes must be positive; intervals must fit int64')
  if len(set(intervals)) != len(intervals):
    parser.error('--intervals must not contain duplicates')

  if args.cpu is not None:
    try:
      allowed_cpus = os.sched_getaffinity(0)
      if args.cpu not in allowed_cpus:
        parser.error('--cpu must be in the current CPU affinity set')
      os.sched_setaffinity(0, {args.cpu})
    except AttributeError:
      parser.error('CPU affinity is unavailable on this platform')

  root = Path(__file__).resolve().parent.parent
  worker = root / 'benchmarks' / 'benchmark_memory_profiler.py'
  environment = os.environ.copy()
  inherited_pythonpath = environment.get('PYTHONPATH')
  environment['PYTHONPATH'] = str(root) + (
      os.pathsep + inherited_pythonpath if inherited_pythonpath else '')
  configurations = [('baseline', 0), ('idle', 0)] + [
      ('active', interval) for interval in intervals]
  rng = random.Random(args.seed)
  runs = {configuration: [] for configuration in configurations}
  paired_baselines = []

  for _ in range(args.repeats):
    order = configurations[:]
    rng.shuffle(order)
    baseline_result = None
    for mode, interval in order:
      command = [
          sys.executable, str(worker), mode,
          '--interval-bytes', str(interval or intervals[0]),
          '--batches', str(args.batches),
          '--operations-per-batch', str(args.operations_per_batch),
          '--case', args.case,
      ]
      process = subprocess.run(
          command, cwd=root, env=environment, check=True, capture_output=True,
          text=True)
      result = json.loads(process.stdout)
      if (mode == 'active' and result['selected_samples'] and
          not result['attributed_objects']):
        raise RuntimeError(
            'Samples have no attributed stacks; check native stack-walking '
            'permissions before interpreting throughput results')
      runs[(mode, interval)].append(result)
      if mode == 'baseline':
        baseline_result = result
    paired_baselines.append(baseline_result)

  cases = ('small', 'varied', 'deep', 'threads_4x') if args.case == 'all' else (
      ('threads_4x',) if args.case == 'threads' else (args.case,))
  summary = {}
  for configuration, results in runs.items():
    label = configuration[0] if configuration[0] != 'active' else (
        'active_%d' % configuration[1])
    summary[label] = {}
    for case in cases:
      throughput = [result[case]['ops_per_second'] for result in results]
      values = {
          'median_ops_per_second': round(statistics.median(throughput)),
          'runs_ops_per_second': [round(value) for value in throughput],
      }
      values['median_batch_p99_ms'] = statistics.median(
          result[case]['batch_p99_ms'] for result in results)
      if configuration[0] != 'baseline':
        paired_losses = [
            100.0 * (1.0 - result[case]['ops_per_second'] /
                     baseline[case]['ops_per_second'])
            for result, baseline in zip(results, paired_baselines)
        ]
        values['median_paired_loss_percent'] = round(
            statistics.median(paired_losses), 2)
        values['paired_loss_percent'] = [round(value, 2)
                                         for value in paired_losses]
      summary[label][case] = values
    if configuration[0] == 'active':
      summary[label]['collection'] = {
          'median_selected_samples': round(statistics.median(
              result['selected_samples'] for result in results)),
          'median_attributed_objects': round(statistics.median(
              result['attributed_objects'] for result in results)),
          'median_trace_count': round(statistics.median(
              result['trace_count'] for result in results)),
      }

  print(json.dumps({
      'seed': args.seed,
      'repeats': args.repeats,
      'batches': args.batches,
      'operations_per_batch': args.operations_per_batch,
      'case': args.case,
      'cpu': args.cpu,
      'python': sys.version,
      'summary': summary,
      'runs': {('%s_%d' % config): results
               for config, results in runs.items()},
  }, indent=2, sort_keys=True))


if __name__ == '__main__':
  main()
