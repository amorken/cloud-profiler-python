"""Regression checks for allocation throughput comparisons."""

from pathlib import Path
import subprocess
import sys

def test_benchmark_rejects_duplicate_intervals_before_running():
  root = Path(__file__).resolve().parent.parent
  result = subprocess.run(
      [sys.executable, str(root / 'benchmarks/compare_memory_throughput.py'),
       '--intervals', '4194304,4194304'], capture_output=True, text=True)
  assert result.returncode == 2
  assert 'duplicates' in result.stderr
