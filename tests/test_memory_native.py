"""Correctness tests for the native allocator sampler and wrappers."""

import json
import math
import os
import subprocess
import sys

import pytest

from googlecloudprofiler import _profiler


def test_poisson_probability_and_estimator_weights():
  selected, probability, objects, byte_estimate = (
      _profiler._memory_test_sampler(50, 100, 50))
  expected_probability = -math.expm1(-0.5)
  assert selected
  assert probability == pytest.approx(expected_probability)
  assert objects == pytest.approx(1.0 / expected_probability)
  assert byte_estimate == pytest.approx(50.0 / expected_probability)

  selected, probability, objects, byte_estimate = (
      _profiler._memory_test_sampler(50, 100, 51))
  assert not selected
  assert (probability, objects, byte_estimate) == (0.0, 0.0, 0.0)

  selected, probability, objects, byte_estimate = (
      _profiler._memory_test_sampler(0, 100, 1))
  assert selected
  assert probability == pytest.approx(-math.expm1(-0.01))
  assert objects == pytest.approx(1.0 / probability)
  assert byte_estimate == 0.0

  # One very large request yields at most one selected request.
  selected, probability, objects, byte_estimate = (
      _profiler._memory_test_sampler(1000000, 10, 0))
  assert selected
  assert probability == 1.0
  assert (objects, byte_estimate) == (1.0, 1000000.0)


def test_memory_hooks_and_storage_are_absent_until_opted_in():
  source = r'''
from googlecloudprofiler import _profiler
assert not _profiler._memory_available()
assert _profiler._memory_test_storage_bytes() == 0
assert _profiler._memory_initialize(524288)
assert _profiler._memory_available()
assert 0 < _profiler._memory_test_storage_bytes() <= 16 * 1024 * 1024
'''
  subprocess.run([sys.executable, '-c', source], check=True)


def test_seeded_sampler_sequences_are_unbiased_over_repeated_seeds():
  interval = 1000
  request_size = 100
  request_count = 50000
  expected_probability = -math.expm1(-request_size / interval)
  seeds = (1, 2, 3, 17, 0x5EED, 0xDEADBEEF)
  total_requests = request_count * len(seeds)
  expected_selected = total_requests * expected_probability
  selected_sigma = math.sqrt(expected_selected * (1 - expected_probability))
  expected_objects = float(total_requests)
  estimator_sigma = math.sqrt(total_requests *
                              (1 - expected_probability) / expected_probability)
  total_selected = 0
  total_objects = 0.0
  total_bytes = 0.0
  for seed in seeds:
    selected, objects, byte_estimate = _profiler._memory_test_sequence(
        request_size, interval, request_count, seed)
    total_selected += selected
    total_objects += objects
    total_bytes += byte_estimate
  # Five standard deviations over all seeded requests avoids an artificially
  # narrow per-seed tolerance while still checking the estimator distribution.
  assert abs(total_selected - expected_selected) <= 5 * selected_sigma
  assert abs(total_objects - expected_objects) <= 5 * estimator_sigma
  assert abs(total_bytes - expected_objects * request_size) <= (
      5 * estimator_sigma * request_size)


def _run_allocator_process(debug_allocator=False):
  source = r'''
from googlecloudprofiler import _profiler
assert _profiler._memory_initialize(1)
assert _profiler._memory_available()
assert _profiler._memory_start()
assert not _profiler._memory_start()
assert _profiler._memory_available()
assert _profiler._memory_test_allocator_calls() == (
    True, True, True, True, True, True, True, True)
assert _profiler._memory_test_callback_state() == (True, True)
assert _profiler._memory_test_nested_hook()
traces, duration_ns, start_time_ns, diagnostics = _profiler._memory_stop()
assert duration_ns >= 0
assert start_time_ns > 0
assert diagnostics['selected_samples'] > 0
assert diagnostics['attributed_objects'] + diagnostics['unknown_objects'] + diagnostics['overflow_objects'] > 0
assert diagnostics['storage_bytes'] > 0
assert diagnostics['export_duration_ns'] >= 0
assert diagnostics['stack_count'] <= diagnostics['stack_capacity']
assert diagnostics['export_frames'] <= diagnostics['export_frame_capacity']
assert traces
assert sum(sample[0] for sample in traces.values()) > 0
assert sum(sample[1] for sample in traces.values()) > 0
'''
  environment = os.environ.copy()
  if debug_allocator:
    environment['PYTHONMALLOC'] = 'debug'
  subprocess.run([sys.executable, '-c', source], check=True, env=environment)


def test_allocator_success_failure_calloc_realloc_zero_and_callback_state():
  _run_allocator_process()


def test_allocator_wrappers_chain_under_debug_allocator():
  _run_allocator_process(debug_allocator=True)


def test_allocator_replacement_disables_future_memory_profiles():
  source = r'''
from googlecloudprofiler import _profiler
assert _profiler._memory_initialize(1)
assert _profiler._memory_start()
assert _profiler._memory_test_replace_allocator()
try:
    _profiler._memory_stop()
except RuntimeError as error:
    assert 'replaced' in str(error)
else:
    raise AssertionError('allocator replacement did not abort collection')
assert not _profiler._memory_available()
assert not _profiler._memory_start()
'''
  subprocess.run([sys.executable, '-c', source], check=True)


def test_fork_child_inherits_inactive_collection():
  source = r'''
import os
from googlecloudprofiler import _profiler
assert _profiler._memory_initialize(1)
assert _profiler._memory_start()
pid = os.fork()
if pid == 0:
    os._exit(0 if (not _profiler._memory_available() and
                   not _profiler._memory_start()) else 1)
_, status = os.waitpid(pid, 0)
assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
_profiler._memory_stop()
'''
  subprocess.run([sys.executable, '-c', source], check=True)


def test_bounded_stack_metadata_and_capacity_overflow():
  source = r'''
import json
from googlecloudprofiler import _profiler

assert _profiler._memory_initialize(1)
namespace = {}
deep_source = 'def recurse(n):\n  return bytearray(3) if n == 0 else recurse(n - 1)\n'
exec(compile(deep_source, 'deep.py', 'exec'), namespace)
long_name = 'fn' + 'n' * 1500
long_filename = '/' + 'p' * 1500
exec(compile('def %s():\n  return bytearray(8)\n' % long_name,
             long_filename, 'exec'), namespace)
line_source = 'def giant():\n' + ''.join(
    '  x = %d\n' % i for i in range(15000)) + '  return bytearray(2)\n'
exec(compile(line_source, 'large_linetable.py', 'exec'), namespace)
for index in range(2200):
    name = 'unique_%04d' % index
    exec(compile('def %s():\n  return bytearray(1)\n' % name,
                 'capacity_smoke.py', 'exec'), namespace)
    namespace.setdefault('_unique_functions', []).append(namespace[name])

line_table = getattr(namespace['giant'].__code__, 'co_linetable', None)
line_budget_supported = line_table is not None and len(line_table) > 65536
if hasattr(namespace['giant'].__code__, 'co_linetable'):
    assert line_budget_supported
assert _profiler._memory_start()
namespace['recurse'](160)
namespace[long_name]()
if line_budget_supported:
    namespace['giant']()
for function in namespace['_unique_functions']:
    function()
traces, duration, start, diagnostics = _profiler._memory_stop()
frames = [frame for trace in traces for frame in trace]
names = [name for name, filename, line in frames]
filenames = [filename for name, filename, line in frames]
print(json.dumps({
    'attributed': diagnostics['attributed_objects'] > 0,
    'stack_marker': any('[stack-truncated]' in name for name in names),
    'line_budget_supported': line_budget_supported,
    'line_marker': any('[line-truncated]' in name for name in names),
    'line_zero': any('[line-truncated]' in name and line == 0
                     for name, filename, line in frames),
    'name_marker': any('[truncated]' in name for name in names),
    'filename_marker': any('[truncated]' in filename for filename in filenames),
    'stack_count': diagnostics['stack_count'],
    'stack_capacity': diagnostics['stack_capacity'],
    'overflow_objects': diagnostics['overflow_objects'],
    'export_frames': diagnostics['export_frames'],
    'export_frame_capacity': diagnostics['export_frame_capacity'],
}))
'''
  result = subprocess.run([sys.executable, '-c', source], check=True,
                          capture_output=True, text=True,
                          env=os.environ.copy())
  report = json.loads(result.stdout)
  if not report['attributed']:
    pytest.skip('process_vm_readv is blocked, so native frame attribution is unavailable')
  assert report['stack_marker']
  if report['line_budget_supported']:
    assert report['line_marker']
    assert report['line_zero']
  assert report['name_marker']
  assert report['filename_marker']
  assert report['stack_count'] == report['stack_capacity']
  assert report['overflow_objects'] > 0
  assert report['export_frames'] <= report['export_frame_capacity']


def test_multithread_allocation_collection_attributes_worker_frames():
  source = r'''
import json
import threading
from googlecloudprofiler import _profiler

assert _profiler._memory_initialize(1)
def worker():
    for unused_index in range(1000):
        bytearray(64)

assert _profiler._memory_start()
workers = [threading.Thread(target=worker) for unused in range(4)]
for thread in workers:
    thread.start()
for thread in workers:
    thread.join()
traces, duration, start, diagnostics = _profiler._memory_stop()
frames = [frame for trace in traces for frame in trace]
print(json.dumps({
    'attributed': diagnostics['attributed_objects'] > 0,
    'worker_frame': any(name == 'worker' for name, filename, line in frames),
    'selected': diagnostics['selected_samples'],
}))
'''
  result = subprocess.run([sys.executable, '-c', source], check=True,
                          capture_output=True, text=True,
                          env=os.environ.copy())
  report = json.loads(result.stdout)
  if not report['attributed']:
    pytest.skip('process_vm_readv is blocked, so native frame attribution is unavailable')
  assert report['selected'] > 0
  assert report['worker_frame']


def test_dynamic_code_objects_can_be_released_after_collection():
  source = r'''
import gc
import json
from googlecloudprofiler import _profiler

assert _profiler._memory_initialize(1)
namespace = {}
filename = 'generated-at-runtime.py'
exec(compile('def runtime_generated_function():\n  return bytearray(64)\n',
             filename, 'exec'), namespace)
function = namespace.pop('runtime_generated_function')
assert _profiler._memory_start()
function()
del function
del namespace
gc.collect()
traces, duration, start, diagnostics = _profiler._memory_stop()
frames = [frame for trace in traces for frame in trace]
print(json.dumps({
    'attributed': diagnostics['attributed_objects'] > 0,
    'dynamic_frame': any(name == 'runtime_generated_function' and
                         filename == 'generated-at-runtime.py'
                         for name, filename, line in frames),
}))
'''
  result = subprocess.run([sys.executable, '-c', source], check=True,
                          capture_output=True, text=True,
                          env=os.environ.copy())
  report = json.loads(result.stdout)
  if not report['attributed']:
    pytest.skip('process_vm_readv is blocked, so native frame attribution is unavailable')
  assert report['dynamic_frame']


def test_collection_boundaries_and_repeated_storage_reuse():
  source = r'''
import threading
import time
from googlecloudprofiler import _profiler

assert _profiler._memory_initialize(1)
storage_bytes = _profiler._memory_test_storage_bytes()
assert storage_bytes > 0

for unused_round in range(4):
    keep_allocating = threading.Event()
    worker_ready = threading.Event()
    keep_allocating.set()

    def worker():
        worker_ready.set()
        while keep_allocating.is_set():
            bytearray(64)

    assert _profiler._memory_start()
    thread = threading.Thread(target=worker)
    thread.start()
    assert worker_ready.wait(5)
    time.sleep(0.01)
    traces, duration, start, diagnostics = _profiler._memory_stop()
    keep_allocating.clear()
    thread.join(5)
    assert not thread.is_alive()
    assert duration >= 0 and start > 0
    assert diagnostics['selected_samples'] > 0
    assert diagnostics['storage_bytes'] == storage_bytes
    assert _profiler._memory_test_storage_bytes() == storage_bytes

# A back-to-back empty collection is valid after repeated populated intervals.
assert _profiler._memory_start()
traces, duration, start, diagnostics = _profiler._memory_stop()
assert isinstance(traces, dict)
assert duration >= 0 and start > 0
assert diagnostics['stack_count'] <= diagnostics['stack_capacity']
assert diagnostics['export_frames'] <= diagnostics['export_frame_capacity']
assert diagnostics['storage_bytes'] == storage_bytes
'''
  subprocess.run([sys.executable, '-c', source], check=True,
                 env=os.environ.copy())


def test_process_shutdown_with_active_allocator_hooks():
  source = r'''
from googlecloudprofiler import _profiler

assert _profiler._memory_initialize(1)
assert _profiler._memory_start()
for unused_index in range(1000):
    bytearray(128)
# Exercise normal interpreter shutdown with process-lifetime wrappers installed
# and a collection active, as can happen when a worker exits between polls.
'''
  subprocess.run([sys.executable, '-c', source], check=True,
                 env=os.environ.copy())
