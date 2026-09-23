"""Checks for comparative allocation benchmark safeguards."""

from pathlib import Path

from benchmarks.compare_memory_revisions import bootstrap_upper, under_root


def test_extension_path_must_be_within_selected_build(tmp_path):
  reference = tmp_path / 'reference'
  reference.mkdir()
  other = tmp_path / 'reference-other'
  other.mkdir()
  assert under_root(reference / 'googlecloudprofiler' / '_profiler.so', reference)
  assert not under_root(other / '_profiler.so', reference)


def test_bootstrap_bound_is_deterministic_and_above_median():
  values = [4, 5, 6, 7, 8, 9, 10, 11, 12, 13]
  first = bootstrap_upper(values, 20260923)
  assert first == bootstrap_upper(values, 20260923)
  assert first >= 8.5
