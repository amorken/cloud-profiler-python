#!/usr/bin/env python3
"""Pair throughput measurements from two separately built profiler trees.

The same workload source from this tree runs in each process. The reference
tree supplies only the imported agent extension. Build both trees in place
with the same Python before running this script.
"""

import argparse
import json
import math
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import tempfile


CASES = ('small', 'varied', 'deep', 'threads')
MODES = ('baseline', 'idle', 'active')


def under_root(path, root):
  return os.path.commonpath((os.path.realpath(path), str(root))) == str(root)


def run_case(root, worker, kind, mode, batches, operations_per_batch,
             interval_bytes):
  environment = os.environ.copy()
  environment['PYTHONPATH'] = str(root)
  command = [sys.executable, str(worker), mode, '--case', kind,
             '--batches', str(batches),
             '--operations-per-batch', str(operations_per_batch),
             '--interval-bytes', str(interval_bytes)]
  result = json.loads(subprocess.run(
      command, cwd=root, env=environment, check=True, capture_output=True,
      text=True).stdout)
  if not under_root(result['extension_path'], root):
    raise RuntimeError('benchmark imported an extension outside ' + str(root))
  if result['diagnostic_stage'] != 4:
    raise RuntimeError('diagnostic extension is not a qualification build')
  if mode == 'active' and (not result['selected_samples'] or
                           not result['attributed_objects']):
    raise RuntimeError('active run lacks attributed native samples')
  key = 'threads_4x' if kind == 'threads' else kind
  return result[key], (result.get('selected_samples', 0),
                       result.get('attributed_objects', 0))


def percentile(values, proportion):
  ordered = sorted(values)
  return ordered[min(len(ordered) - 1,
                     math.ceil(proportion * len(ordered)) - 1)]


def bootstrap_upper(values, seed, draws=10000):
  randomizer = random.Random(seed)
  medians = [statistics.median(
      randomizer.choices(values, k=len(values))) for _ in range(draws)]
  return percentile(medians, 0.975)


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--reference-root', type=Path, required=True)
  parser.add_argument('--candidate-root', type=Path,
                      default=Path(__file__).resolve().parent.parent)
  parser.add_argument('--interval-bytes', type=int, default=4 * 1024 * 1024)
  parser.add_argument('--repeats', type=int, default=10)
  parser.add_argument('--target-seconds', type=float, default=3.0)
  parser.add_argument('--operations-per-batch', type=int, default=1000)
  parser.add_argument('--seed', type=int, default=20260923)
  parser.add_argument('--cpu', type=int)
  parser.add_argument('--output', type=Path,
                      help='write results after each completed workload')
  args = parser.parse_args()
  if (args.repeats < 2 or not math.isfinite(args.target_seconds) or
      args.target_seconds <= 0 or
      args.operations_per_batch <= 0 or args.interval_bytes <= 0):
    parser.error('invalid repeat, duration, operation, or interval count')
  roots = {name: root.resolve() for name, root in (
      ('reference', args.reference_root),
      ('candidate', args.candidate_root))}
  if roots['reference'] == roots['candidate']:
    parser.error('reference and candidate trees must differ')
  if args.cpu is not None:
    if args.cpu not in os.sched_getaffinity(0):
      parser.error('--cpu is outside the allowed affinity set')
    os.sched_setaffinity(0, {args.cpu})

  worker = (roots['candidate'] / 'benchmarks' /
            'benchmark_memory_profiler.py')
  randomizer = random.Random(args.seed)
  measurements = {}
  for case in CASES:
    calibration, unused = run_case(
        roots['reference'], worker, case, 'baseline', 1000,
        args.operations_per_batch, args.interval_bytes)
    batches = max(1000, math.ceil(1000 * args.target_seconds /
                                  calibration['seconds']))
    rows = []
    measurements[case] = {'batches': batches, 'rows': rows}
    for repeat in range(args.repeats):
      order = [(tree, mode) for tree in roots for mode in MODES]
      randomizer.shuffle(order)
      results = {}
      for tree, mode in order:
        measured, diagnostics = run_case(
            roots[tree], worker, case, mode, batches,
            args.operations_per_batch, args.interval_bytes)
        results[(tree, mode)] = dict(measured, diagnostics=diagnostics)
      reference_baseline = results[('reference', 'baseline')]
      candidate_baseline = results[('candidate', 'baseline')]
      reference_active = results[('reference', 'active')]
      candidate_active = results[('candidate', 'active')]
      rows.append({
          'repeat': repeat,
          'reference': {mode: results[('reference', mode)] for mode in MODES},
          'candidate': {mode: results[('candidate', mode)] for mode in MODES},
          'reference_active_loss_percent': 100 * (
              1 - reference_active['ops_per_second'] /
              reference_baseline['ops_per_second']),
          'candidate_active_loss_percent': 100 * (
              1 - candidate_active['ops_per_second'] /
              candidate_baseline['ops_per_second']),
          'candidate_idle_loss_percent': 100 * (
              1 - results[('candidate', 'idle')]['ops_per_second'] /
              candidate_baseline['ops_per_second']),
          'candidate_active_p99_change_percent': 100 * (
              candidate_active['batch_p99_ms'] /
              reference_active['batch_p99_ms'] - 1),
      })
      if args.output:
        write_results(args.output, args, roots, measurements)
    fields = ('reference_active_loss_percent',
              'candidate_active_loss_percent', 'candidate_idle_loss_percent',
              'candidate_active_p99_change_percent')
    measurements[case]['summary'] = {
        field: {
            'median': statistics.median(row[field] for row in rows),
            'bootstrap_upper_95': bootstrap_upper(
                [row[field] for row in rows],
                args.seed + CASES.index(case) * 10 + fields.index(field)),
        } for field in fields
    }
    if args.output:
      write_results(args.output, args, roots, measurements)
  print(json.dumps(report(args, roots, measurements), indent=2, sort_keys=True))


def report(args, roots, measurements):
  return {
      'reference_root': str(roots['reference']),
      'candidate_root': str(roots['candidate']),
      'python': sys.version,
      'cpu_affinity': sorted(os.sched_getaffinity(0)),
      'interval_bytes': args.interval_bytes,
      'repeats': args.repeats,
      'target_seconds': args.target_seconds,
      'seed': args.seed,
      'cases': measurements,
  }


def write_results(path, args, roots, measurements):
  path = path.resolve()
  with tempfile.NamedTemporaryFile(
      mode='w', dir=path.parent, prefix=path.name + '.',
      delete=False) as output:
    json.dump(report(args, roots, measurements), output, indent=2,
              sort_keys=True)
    temporary = output.name
  os.replace(temporary, path)


if __name__ == '__main__':
  main()
