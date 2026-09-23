"""pprof encoding and memory-profiler integration tests."""

import gzip
import os
import subprocess
import sys

import pytest

from googlecloudprofiler import builder
from googlecloudprofiler import profile_pb2


def _decoded_profile(compressed):
  profile = profile_pb2.Profile()
  profile.ParseFromString(gzip.decompress(compressed))
  return profile


def test_memory_profile_uses_weighted_values_and_leaf_first_frames():
  profile_builder = builder.Builder()
  traces = {
      (('leaf', 'sample.py', 11), ('caller', 'sample.py', 5)): (3, 700),
  }
  profile_builder.populate_memory_profile(traces, 524288, 123456789,
                                          1700000000000000000)
  profile = _decoded_profile(profile_builder.emit())
  strings = profile.string_table

  assert [(strings[value.type], strings[value.unit])
          for value in profile.sample_type] == [
              ('alloc_objects', 'count'), ('alloc_space', 'bytes')]
  assert strings[profile.period_type.type] == 'alloc_space'
  assert strings[profile.period_type.unit] == 'bytes'
  assert profile.period == 524288
  assert profile.duration_nanos == 123456789
  assert profile.time_nanos == 1700000000000000000
  assert strings[profile.default_sample_type] == 'alloc_space'
  assert list(profile.sample[0].value) == [3, 700]
  leaf_location, caller_location = profile.sample[0].location_id
  assert strings[profile.function[profile.location[leaf_location - 1].line[0].function_id - 1].name] == 'leaf'
  assert strings[profile.function[profile.location[caller_location - 1].line[0].function_id - 1].name] == 'caller'


def test_memory_profile_includes_bounded_collection_diagnostics_comment():
  profile_builder = builder.Builder()
  diagnostics = {
      'selected_samples': 8,
      'attributed_objects': 6.0,
      'attributed_bytes': 1200.0,
      'unknown_objects': 1.0,
      'unknown_bytes': 200.0,
      'overflow_objects': 1.0,
      'overflow_bytes': 200.0,
      'stack_count': 4,
      'stack_capacity': 2048,
      'export_frames': 12,
      'export_frame_capacity': 65536,
      'string_bytes': 900,
      'string_bytes_capacity': 4194304,
      'storage_bytes': 13246560,
      'collection_duration_ns': 1000000,
      'export_duration_ns': 12000,
  }
  profile_builder.populate_memory_profile({}, 524288, 1000000, 1700000000,
                                          diagnostics)
  profile = _decoded_profile(profile_builder.emit())
  comment = profile.string_table[profile.comment[0]]
  assert 'selected=8' in comment
  assert 'unknown=1.0/200.0' in comment
  assert 'overflow=1.0/200.0' in comment
  assert 'stacks=4/2048' in comment
  assert 'export_ns=12000' in comment


def test_memory_profile_rejects_invalid_or_overflowing_estimates():
  invalid_traces = [
      {(('f', 'x.py', 1),): (1, -1)},
      {(('f', 'x.py', 1),): (1 << 63, 1)},
      {(('f', 'x.py', 1),): (1,)},
  ]
  for traces in invalid_traces:
    profile_builder = builder.Builder()
    with pytest.raises(ValueError):
      profile_builder.populate_memory_profile(traces, 1, 0, 1)


def test_empty_builder_profile_has_valid_memory_metadata():
  profile_builder = builder.Builder()
  profile_builder.populate_memory_profile({}, 524288, 0,
                                          1700000000000000000)
  profile = _decoded_profile(profile_builder.emit())
  strings = profile.string_table
  assert not profile.sample
  assert len(profile.sample_type) == 2
  assert strings[profile.default_sample_type] == 'alloc_space'
  assert profile.period == 524288
  assert profile.duration_nanos == 0
  assert profile.time_nanos == 1700000000000000000


def test_empty_native_memory_interval_emits_valid_gzip_profile():
  source = r'''
import gzip
from googlecloudprofiler.memory_profiler import MemoryProfiler
from googlecloudprofiler.profile_pb2 import Profile
profiler = MemoryProfiler(524288)
assert profiler.available
compressed = profiler.profile(0)
profile = Profile()
profile.ParseFromString(gzip.decompress(compressed))
assert profile.period == 524288
assert profile.duration_nanos >= 0
assert profile.time_nanos > 0
assert len(profile.sample_type) == 2
assert profile.default_sample_type < len(profile.string_table)
'''
  subprocess.run([sys.executable, '-c', source], check=True,
                 env=os.environ.copy())


def test_cpu_profile_smoke_after_shared_frame_walker_change():
  if not sys.platform.startswith('linux'):
    pytest.skip('native CPU profiler is Linux-only')
  source = r'''
import gzip
from googlecloudprofiler.cpu_profiler import CPUProfiler
from googlecloudprofiler.profile_pb2 import Profile

compressed = CPUProfiler(period_ms=1).profile(50000000)
profile = Profile()
profile.ParseFromString(gzip.decompress(compressed))
strings = profile.string_table
assert profile.period == 1000000
assert profile.duration_nanos == 50000000
assert [(strings[item.type], strings[item.unit])
        for item in profile.sample_type] == [
            ('sample', 'count'), ('CPU', 'nanoseconds')]
# An empty sample set is expected in the restricted sandbox, where
# process_vm_readv is denied while the shared native frame walker runs.
'''
  subprocess.run([sys.executable, '-c', source], check=True,
                 env=os.environ.copy())
