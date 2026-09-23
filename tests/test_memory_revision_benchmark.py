"""Checks for comparative allocation benchmark safeguards."""

from pathlib import Path

import json

import pytest

from benchmarks.build_memory_variant import digest_file, source_digest
from benchmarks.compare_memory_revisions import (
    bootstrap_bounds, load_manifest, under_root)


def test_extension_path_must_be_within_selected_build(tmp_path):
  reference = tmp_path / 'reference'
  reference.mkdir()
  other = tmp_path / 'reference-other'
  other.mkdir()
  assert under_root(reference / 'googlecloudprofiler' / '_profiler.so', reference)
  assert not under_root(other / '_profiler.so', reference)


def test_bootstrap_bound_is_deterministic_and_above_median():
  values = [4, 5, 6, 7, 8, 9, 10, 11, 12, 13]
  lower, upper = bootstrap_bounds(values, 20260923)
  assert (lower, upper) == bootstrap_bounds(values, 20260923)
  assert lower <= 8.5 <= upper


def test_build_manifest_rejects_changed_sources_and_extension(tmp_path):
  (tmp_path / 'googlecloudprofiler' / 'src').mkdir(parents=True)
  (tmp_path / 'setup.py').write_text('original')
  extension = tmp_path / 'googlecloudprofiler' / '_profiler.so'
  extension.write_bytes(b'original')
  import sys
  manifest = {
      'python': str(Path(sys.executable).resolve()),
      'source_sha256': source_digest(tmp_path),
      'extension': str(extension.relative_to(tmp_path)),
      'extension_sha256': digest_file(extension),
      'commands': {'compile': [], 'link': []},
  }
  (tmp_path / 'memory-build-manifest.json').write_text(json.dumps(manifest))
  assert load_manifest(tmp_path) == manifest
  (tmp_path / 'setup.py').write_text('changed')
  with pytest.raises(RuntimeError, match='sources changed'):
    load_manifest(tmp_path)
  (tmp_path / 'setup.py').write_text('original')
  extension.write_bytes(b'changed')
  with pytest.raises(RuntimeError, match='extension changed'):
    load_manifest(tmp_path)
