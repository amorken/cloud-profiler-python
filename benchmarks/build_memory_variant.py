#!/usr/bin/env python3
"""Build an isolated profiler tree and record the exact native build inputs."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys


NATIVE_SOURCES = ('setup.py',)
MANIFEST_NAME = 'memory-build-manifest.json'


def digest_file(path):
  return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest(root):
  paths = [root / name for name in NATIVE_SOURCES]
  paths.extend(sorted((root / 'googlecloudprofiler' / 'src').glob('*.cc')))
  paths.extend(sorted((root / 'googlecloudprofiler' / 'src').glob('*.h')))
  digest = hashlib.sha256()
  for path in paths:
    digest.update(str(path.relative_to(root)).encode())
    digest.update(b'\0')
    digest.update(path.read_bytes())
    digest.update(b'\0')
  return digest.hexdigest()


def build_commands(output):
  """Keep flags from actual compiler invocations, independent of object order."""
  compile_flags = []
  link_flags = []
  for line in output.splitlines():
    if not line.startswith(('c++ ', 'cc ', 'g++ ', 'gcc ', 'clang++ ',
                            'clang ')):
      continue
    parts = shlex.split(line)
    if not parts or parts[0] not in ('c++', 'cc', 'g++', 'gcc', 'clang++',
                                    'clang'):
      continue
    if '-c' in parts and '-o' in parts:
      source = parts[parts.index('-c') + 1]
      ignored = {parts.index('-c'), parts.index('-c') + 1,
                 parts.index('-o'), parts.index('-o') + 1}
      compile_flags.append((source, [part for index, part in enumerate(parts)
                                    if index not in ignored]))
    elif '-shared' in parts:
      ignored = set()
      if '-o' in parts:
        ignored.update((parts.index('-o'), parts.index('-o') + 1))
      link_flags = [part for index, part in enumerate(parts)
                    if index not in ignored and not part.endswith('.o')]
  if not compile_flags or not link_flags:
    raise RuntimeError('build output contains no complete compiler/linker commands')
  return dict(compile=sorted(compile_flags), link=link_flags)


def build(root, python):
  root = root.resolve()
  python = python.resolve()
  result = subprocess.run(
      [str(python), 'setup.py', 'build_ext', '--inplace', '--force'],
      cwd=root, capture_output=True, text=True)
  if result.returncode:
    raise RuntimeError(result.stdout + result.stderr)
  commands = build_commands(result.stdout + result.stderr)
  version = subprocess.check_output(
      [str(python), '-c', 'import sys; print(sys.version)'], text=True).strip()
  suffix = subprocess.check_output(
      [str(python), '-c',
       'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))'],
      text=True).strip()
  extensions = list((root / 'googlecloudprofiler').glob('_profiler*' + suffix))
  if len(extensions) != 1:
    raise RuntimeError('expected exactly one built profiler extension')
  extension = extensions[0]
  manifest = {
      'python': str(python),
      'python_version': version,
      'source_sha256': source_digest(root),
      'extension': str(extension.relative_to(root)),
      'extension_sha256': digest_file(extension),
      'commands': commands,
  }
  (root / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2,
                                              sort_keys=True) + '\n')
  return manifest


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--root', type=Path, required=True)
  parser.add_argument('--python', type=Path, default=Path(sys.executable))
  args = parser.parse_args()
  print(json.dumps(build(args.root, args.python), indent=2, sort_keys=True))


if __name__ == '__main__':
  main()
