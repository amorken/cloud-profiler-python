"""Agent scheduling and upload integration for allocation profiles."""

import base64
import inspect
import sys
import pytest

import googlecloudprofiler
from googlecloudprofiler import client
from googlecloudprofiler import memory_profiler


def test_default_memory_sampling_interval_is_four_mib():
  expected = 4 * 1024 * 1024
  assert memory_profiler.DEFAULT_SAMPLING_INTERVAL_BYTES == expected
  assert inspect.signature(googlecloudprofiler.start).parameters[
      'memory_sampling_interval_bytes'].default == expected
  assert inspect.signature(client.Client.config).parameters[
      'memory_sampling_interval_bytes'].default == expected
  assert inspect.signature(memory_profiler.MemoryProfiler).parameters[
      'sampling_interval_bytes'].default == expected


class _Request:

  def __init__(self, response=None):
    self.response = response
    self.execute_calls = []

  def execute(self, **kwargs):
    self.execute_calls.append(kwargs)
    return self.response


class _Profiles:

  def __init__(self):
    self.create_request = _Request({'name': 'projects/p/profiles/1'})
    self.patch_request = _Request()
    self.create_arguments = None
    self.patch_arguments = None

  def create(self, **kwargs):
    self.create_arguments = kwargs
    return self.create_request

  def patch(self, **kwargs):
    self.patch_arguments = kwargs
    return self.patch_request


class _ProfilerService:

  def __init__(self, profiles):
    self._profiles = profiles

  def projects(self):
    return self

  def profiles(self):
    return self._profiles

  def create(self, **kwargs):
    return self._profiles.create(**kwargs)

  def patch(self, **kwargs):
    return self._profiles.patch(**kwargs)


class _MemoryProfiler:

  def __init__(self, profile_bytes=b'compressed-profile', available=True,
               error=None):
    self._profile_bytes = profile_bytes
    self.available = available
    self.error = error

  def profile(self, unused_duration_ns):
    if self.error is not None:
      raise self.error
    return self._profile_bytes


def test_schedule_requests_heapprofiler_type():
  profiles = _Profiles()
  agent = client.Client.__new__(client.Client)
  agent._deployment = {'projectId': 'p', 'target': 'service', 'labels': {}}
  agent._profilers = {'CPU': object(), 'WALL': object(),
                      'HEAP_ALLOC': _MemoryProfiler()}
  agent._profiler_service = _ProfilerService(profiles)

  response = agent._create_profile()

  assert response['name'] == 'projects/p/profiles/1'
  assert profiles.create_arguments['body']['profileType'] == [
      'CPU', 'WALL', 'HEAP_ALLOC']


def test_heapprofiler_upload_uses_existing_patch_flow():
  profiles = _Profiles()
  agent = client.Client.__new__(client.Client)
  agent._profilers = {'HEAP_ALLOC': _MemoryProfiler()}
  agent._profiler_service = _ProfilerService(profiles)
  profile = {
      'name': 'projects/p/profiles/1',
      'profileType': 'HEAP_ALLOC',
      'duration': '0.001s',
  }

  agent._collect_and_upload_profile(profile)

  assert profiles.patch_arguments['name'] == profile['name']
  assert base64.b64decode(profiles.patch_arguments['body']['profileBytes']) == (
      b'compressed-profile')
  assert profiles.patch_request.execute_calls == [{'num_retries': 3}]


def test_unavailable_memory_profiler_is_removed_without_removing_cpu():
  agent = client.Client.__new__(client.Client)
  profiles = _Profiles()
  agent._profilers = {
      'CPU': object(), 'WALL': object(),
      'HEAP_ALLOC': _MemoryProfiler(available=False,
                                   error=RuntimeError('allocator replaced')),
  }
  agent._deployment = {'projectId': 'p', 'target': 'service', 'labels': {}}
  agent._profiler_service = _ProfilerService(profiles)
  profile = {
      'name': 'projects/p/profiles/1',
      'profileType': 'HEAP_ALLOC',
      'duration': '0.001s',
  }

  agent._collect_and_upload_profile(profile)

  assert list(agent._profilers) == ['CPU', 'WALL']
  agent._create_profile()
  assert profiles.create_arguments['body']['profileType'] == ['CPU', 'WALL']


@pytest.mark.skipif(not sys.platform.startswith('linux'),
                    reason='native allocation profiling is Linux-only')
def test_memory_profiler_is_registered_only_after_native_setup(monkeypatch):
  agent = client.Client.__new__(client.Client)
  agent._profilers = {}
  available_profiler = _MemoryProfiler(available=True)
  monkeypatch.setattr(client.memory_profiler, 'MemoryProfiler',
                      lambda interval: available_profiler)

  agent._config_memory_profiling(12345)

  assert agent._profilers == {'HEAP_ALLOC': available_profiler}

  unavailable_agent = client.Client.__new__(client.Client)
  unavailable_agent._profilers = {}
  unavailable_profiler = _MemoryProfiler(available=False)
  monkeypatch.setattr(client.memory_profiler, 'MemoryProfiler',
                      lambda interval: unavailable_profiler)

  unavailable_agent._config_memory_profiling(12345)

  assert unavailable_agent._profilers == {}


def test_polling_stops_if_no_profile_types_remain():
  agent = client.Client.__new__(client.Client)
  agent._profilers = {}

  agent._poll_profiler_service()


def test_public_memory_options_validate_before_auth_or_hook_install(monkeypatch):
  import googlecloudprofiler

  auth_calls = []
  monkeypatch.setattr(googlecloudprofiler, '_started', False)
  monkeypatch.setattr(
      client.Client, 'setup_auth',
      lambda *unused_args, **unused_kwargs: auth_calls.append(True))
  with pytest.raises(ValueError, match='enable_memory_profiling'):
    googlecloudprofiler.start(enable_memory_profiling=1)
  with pytest.raises(ValueError, match='sampling_interval_bytes'):
    googlecloudprofiler.start(memory_sampling_interval_bytes=0)
  assert not auth_calls
