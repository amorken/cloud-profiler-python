# Google Cloud Python profiling agent

Python profiling agent for
[Google Cloud Profiler](https://cloud.google.com/profiler/).

See
[Google Cloud Profiler profiling Python code](https://cloud.google.com/profiler/docs/profiling-python)
for detailed documentation.

## Supported OS

Linux. Profiling Python applications is supported for Linux kernels whose
standard C library is implemented with `glibc` or with `musl`. For configuration
information specific to Linux Alpine kernels, see
[Running on Linux Alpine](https://cloud.google.com/profiler/docs/profiling-python#running_with_linux_alpine).

## Supported Python Versions

Python >= 3.7 and <= 3.13

## Installation & usage

1.  Install the profiler package using PyPI:

    ```shell
    pip3 install google-cloud-profiler
    ```

2.  Enable the profiler in your application:

    ```python
    import googlecloudprofiler

    def main():
        # Profiler initialization. It starts a daemon thread which continuously
        # collects and uploads profiles. Best done as early as possible.
        try:
            googlecloudprofiler.start(
                service='hello-profiler',
                service_version='1.0.1',
                # verbose is the logging level. 0-error, 1-warning, 2-info,
                # 3-debug. It defaults to 0 (error) if not set.
                verbose=3,
                # project_id must be set if not running on GCP.
                # project_id='my-project-id',
            )
        except (ValueError, NotImplementedError) as exc:
            print(exc)  # Handle errors here
    ```

### Allocation profiles

`enable_memory_profiling` opts in to `HEAP_ALLOC` profiles. It defaults to
`False`. The profiler samples successful allocation requests in CPython's
`PYMEM_DOMAIN_MEM` and `PYMEM_DOMAIN_OBJ` allocators, then weights each selected
request using a byte-based Poisson sampler. For a request of size `s` and mean
interval `R`, its inclusion probability is
`1 - exp(-max(s, 1) / R)`. A selected request contributes `1/p` estimated
objects and `s/p` estimated bytes. The configured interval must be a positive
integer and remains fixed for each collection.

This path is experimental and is not production-qualified. Current local
measurements exceed the active-collection performance gate; see the
implementation record in `memprofile.md` for the results and outstanding
qualification work. Enable it explicitly only for evaluation:

```python
googlecloudprofiler.start(
    service='hello-profiler',
    service_version='1.0.1',
    enable_memory_profiling=True,
    memory_sampling_interval_bytes=524288,
)
```

This profile measures estimated allocation traffic during the interval,
including allocations that were freed before it ended. It does not report live
heap size. Reallocations count once at the full new requested size; frees do not
count. `calloc` uses the requested element count times element size when that
product is representable. Successful zero-byte requests can contribute an
object estimate with zero bytes. Failed allocations do not count or advance the
sampler.

Memory profiles require the Linux native profiler runtime, an attached thread
in the main interpreter, and the GIL. Free-threaded CPython builds and
subinterpreters are unsupported. The agent does not profile the RAW allocator
domain, direct native allocations, or Python freelists. Per-stack capture is
limited to 128 frames; collection storage
is capped at 2,048 stacks and 16 MiB, and profile export has a bounded frame
budget. Missing stacks and capacity overflow are retained in explicit unknown
and overflow buckets. Names and filenames are copied into owned storage with a
1,024-byte limit per field; oversized line tables use line zero and a visible
truncation marker. The collector resolves at most 1,000,000 lines per
collection, accepts line tables up to 64 KiB, and caps aggregate line-table
work at 16 MiB. Profile export is capped at 65,536 frames.

Allocator wrappers remain installed for the process lifetime after memory
profiling is first enabled. If another component replaces either wrapped
allocator, the agent skips that collection and removes later `HEAP_ALLOC`
requests while CPU and wall profiling continue. For worker processes, start the
agent after forking. Restarting an agent inherited from a process that already
started it is unsupported in this version.

## Installation on Linux Alpine

The Python profiling agent has a native component. The base Alpine image for
Python does not have all dependencies required to build this native component
installed. To build the Python profiling agent on Alpine, one must install the
package `build-base`.

To use the Python profiling agent on Alpine without installing additional
dependencies on to the final Alpine image, one can use a two-stage build and
compile the Python profiling agent in the first stage.

Here is an example of a Docker image that uses a multi-stage build to compile
and install the Python profiling agent:

```
FROM python:3.7-alpine as builder

# Install build-base to allow for compilation of the profiling agent.
RUN apk add --update --no-cache build-base

# Compile the profiling agent, generating wheels for it.
RUN pip3 wheel --wheel-dir=/tmp/wheels google-cloud-profiler


FROM python:3.7-alpine

# Copy over the directory containing wheels for the profiling agent.
COPY --from=builder /tmp/wheels /tmp/wheels

# Install the profiling agent.
RUN pip3 install --no-index --find-links=/tmp/wheels google-cloud-profiler

# Install any other required modules or dependencies, and copy an app which
# enables the profiler as described in "Enable the profiler in your
# application".
COPY ./bench.py .

# Run the application when the docker image is run, using either CMD (as is done
# here) or ENTRYPOINT.
CMD python3 -u bench.py
```


## Troubleshooting

### Resource temporarily unavailable errors with Python

If you see the following log entries after enabling the Profiler:

```
BlockingIOError: [Errno 11] Resource temporarily unavailable
Exception ignored when trying to write to the signal wakeup fd
```

see https://cloud.google.com/profiler/docs/troubleshooting#python-blocking for
the cause and the workaround.
