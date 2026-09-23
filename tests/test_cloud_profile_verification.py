"""Verify server readback comparisons preserve allocation stack attribution."""

import base64
import gzip

from benchmarks.verify_cloud_profiles import validate
from googlecloudprofiler import builder, memory_profiler, profile_pb2


def _profile():
  output = builder.Builder()
  output.populate_memory_profile(
      {(('allocation_leaf', 'load.py', 12), ('caller', 'load.py', 5)): (4, 256)},
      memory_profiler.DEFAULT_SAMPLING_INTERVAL_BYTES, 1000000, 123)
  proto = profile_pb2.Profile()
  proto.ParseFromString(gzip.decompress(output.emit()))
  return proto


def _summary(proto):
  return validate({
      'profileType': 'HEAP_ALLOC',
      'profileBytes': base64.b64encode(
          gzip.compress(proto.SerializeToString())).decode(),
  })


def test_readback_comparison_tolerates_renumbered_and_merged_samples():
  proto = _profile()
  expected = _summary(proto)
  for function in proto.function:
    function.id += 10
  for location in proto.location:
    location.id += 20
    for line in location.line:
      line.function_id += 10
  for sample in proto.sample:
    sample.location_id[:] = [location + 20 for location in sample.location_id]
  sample = proto.sample[0]
  sample.value[:] = [2, 128]
  proto.sample.add().CopyFrom(sample)
  assert _summary(proto) == expected


def test_readback_comparison_detects_changed_stack_with_identical_totals():
  proto = _profile()
  expected = _summary(proto)
  proto.location[-1].line[0].line += 1
  actual = _summary(proto)
  assert actual['totals'] == expected['totals']
  assert actual['samples'] == expected['samples']
  assert actual['stack_values_sha256'] != expected['stack_values_sha256']
