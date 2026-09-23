"""Explicit opt-in live CPU/HEAP_ALLOC upload and server readback smoke test.

Run from the repository with PYTHONPATH=. and a built native extension.
Uses the active gcloud login; never writes or prints its access token.
Creates profiles until CPU and HEAP_ALLOC are collected, under a unique service.
"""

import argparse
import base64
import datetime
import gzip
import hashlib
import json
import logging
import signal
import subprocess
import sys
import threading
import time
import uuid

from google.oauth2.credentials import Credentials
from googlecloudprofiler import client, memory_profiler, profile_pb2


def allocation_leaf():
  return [bytearray(4096 + i) for i in range(256)]


def allocation_workload(stop):
  while not stop.is_set():
    objects = allocation_leaf()
    sum(len(obj) for obj in objects)


def validate(profile):
  proto = profile_pb2.Profile()
  proto.ParseFromString(gzip.decompress(base64.b64decode(profile['profileBytes'])))
  strings = proto.string_table
  types = [(strings[t.type].lower(), strings[t.unit]) for t in proto.sample_type]
  functions = {f.id: (strings[f.name], strings[f.filename])
               for f in proto.function}
  locations = {loc.id: [(*functions[line.function_id], line.line)
                        for line in loc.line] for loc in proto.location}
  if profile['profileType'] == 'HEAP_ALLOC':
    if types != [('alloc_objects', 'count'), ('alloc_space', 'bytes')]:
      raise RuntimeError('Unexpected allocation sample schema')
    if proto.period != memory_profiler.DEFAULT_SAMPLING_INTERVAL_BYTES:
      raise RuntimeError('Unexpected sampling interval')
  elif profile['profileType'] != 'CPU' or types != [
      ('sample', 'count'), ('cpu', 'nanoseconds')]:
    raise RuntimeError('Unexpected CPU sample schema')
  # Resolve IDs and aggregate equal stacks before hashing: the backend can
  # reorder tables, renumber IDs, and merge samples without changing attribution.
  stacks = {}
  for sample in proto.sample:
    if len(sample.value) != len(types) or any(v < 0 for v in sample.value):
      raise RuntimeError('Invalid sample values')
    stack = tuple(frame for loc in sample.location_id for frame in locations[loc])
    totals = stacks.setdefault(stack, [0] * len(types))
    for index, value in enumerate(sample.value):
      totals[index] += value
  workload_samples = [values for stack, values in stacks.items()
                      if any(frame[0] in ('allocation_leaf', 'allocation_workload')
                             for frame in stack)]
  if not any(sum(values) > 0 for values in workload_samples):
    raise RuntimeError('No positive samples attributed to the workload')
  signature = hashlib.sha256(
      json.dumps(sorted(stacks.items())).encode()).hexdigest()
  return dict(profile_type=profile['profileType'], stack_values_sha256=signature,
              sample_types=types, samples=len(stacks),
              workload_samples=len(workload_samples), period=proto.period,
              totals=[sum(s.value[i] for s in proto.sample)
                      for i in range(len(types))])


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--project', required=True)
  parser.add_argument('--gcloud', default='gcloud')
  args = parser.parse_args()
  logging.basicConfig(level=logging.INFO)
  # A process-wide bound also covers server scheduling and delayed readback.
  signal.alarm(300)
  token = subprocess.check_output(
      [args.gcloud, 'auth', 'print-access-token'], text=True).strip()
  agent = client.Client()
  agent._credentials = Credentials(token)
  service = 'heap-sampler-live-' + datetime.datetime.now(
      datetime.timezone.utc).strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8]
  agent.config(
      project_id=args.project, service=service,
      service_version='native-sampler-py%d%d' % sys.version_info[:2],
      disable_cpu_profiling=False, disable_wall_profiling=True, period_ms=10,
      discovery_service_url=None, enable_memory_profiling=True)
  agent._profiler_service = agent._build_service()
  if set(agent._profilers) != {'CPU', 'HEAP_ALLOC'}:
    raise RuntimeError('Both native profilers must be available')
  print(json.dumps(dict(project=args.project, service=service)), flush=True)
  expected = {}
  stop = threading.Event()
  worker = threading.Thread(target=allocation_workload, args=(stop,))
  worker.start()
  try:
    collected = set()
    while collected != {'CPU', 'HEAP_ALLOC'}:
      profile = agent._create_profile()
      kind = profile['profileType']
      agent._collect_and_upload_profile(profile)
      expected[profile['name']] = validate(profile)
      collected.add(kind)
      print(json.dumps(dict(uploaded=profile['name'],
                            **expected[profile['name']])), flush=True)
  finally:
    stop.set()
    worker.join()
  # The agent catches upload errors; verify decoded server data independently.
  # The backend can re-encode profiles and omits names in list responses.
  verified = {}
  for _ in range(12):
    request = agent._profiler_service.list(parent='projects/' + args.project,
                                           pageSize=1000)
    while request is not None:
      response = request.execute(num_retries=3)
      for profile in response.get('profiles', []):
        if profile.get('deployment', {}).get('target') != service:
          continue
        if not profile.get('profileBytes'):
          continue
        summary = validate(profile)
        for name, uploaded_summary in expected.items():
          if summary == uploaded_summary:
            verified[name] = dict(name=name, **summary)
      request = agent._profiler_service.list_next(request, response)
    if len(verified) == len(expected):
      print(json.dumps(dict(verified=list(verified.values())), indent=2), flush=True)
      signal.alarm(0)
      return
    time.sleep(5)
  raise RuntimeError('Server readback did not verify both uploaded profiles')


if __name__ == '__main__':
  main()
