# Two threads doing MEMFS I/O deadlock: both parked in `memory.atomic.wait32`, main thread idle

Under `-pthread`, a program with two threads doing ordinary file I/O stops making progress after
a few hundred thousand operations. Both worker threads are parked in `memory.atomic.wait32`
waiting for a synchronously proxied syscall to come back, while the main thread sits in its event
loop with an empty queue and keeps running normally. It never recovers; we have left it wedged for
10 minutes.

Reproducer, one file, no dependencies: **https://github.com/MeshInspector/wasm-memfs-stall**

```
docker run --rm -v "$PWD:/src" -w /src emscripten/emsdk:4.0.19-arm64 \
  em++ -std=c++20 -O2 -pthread memfs_repro.cpp -DRUN_SECONDS=480 \
  -s PTHREAD_POOL_SIZE=2 -s PTHREAD_POOL_SIZE_STRICT=0 -s ALLOW_MEMORY_GROWTH=1 \
  -s EXIT_RUNTIME=1 -s ENVIRONMENT=node,worker -o out/repro.js
taskset -c 0,1 node out/repro.js
```

`main()` installs `emscripten_set_main_loop(frame, 0, 0)` and returns, so the main thread is back in
the event loop between frames. Two detached threads then run until the time is up:

- a **copier**: `unlink` the destination, `open` a 100 KB source, `open` the destination, `read`,
  `write`, `close`, `close`, repeat;
- a **writer**: `write` one ~54-byte line to a log file opened `O_APPEND`, repeat.

The main loop watches a progress counter and prints `STALLED` when it has not moved for 60 s, naming
the syscall each thread was about to enter.

## How often

About **one shard in seven** hangs, where a shard is 480 s on a 2-core `ubuntu-24.04-arm` GitHub
runner: 7/48, 7/48, 2/12, 1/12, 2/12, 2/12 across runs. A clean shard completes ~1.3 M copies, so
the rate is roughly **one hang per 10 million copies** (~70 million proxied syscalls). It is bursty:
runs of 12 shards fairly often produce none.

## The stacks

`gdb -p <node> -batch -ex "thread apply all bt"` on a wedged process, 5 captures, all alike:

```
Thread 1 "node": __GI_epoll_pwait () / uv__io_poll () / uv_run ()      <- idle, nothing queued
Thread 2 "node": Builtins_WasmCEntry () / Runtime_WasmI32AtomicWait ()
                 / v8::internal::FutexEmulation::WaitWasm32 ()
Thread 3 "node": (identical to thread 2)
```

9 of 10 worker samples are in `WaitWasm32`; the tenth was in JIT code. gdb cannot unwind wasm
frames, so the C++ side is invisible, but the V8 frames are enough to place both workers in
`memory.atomic.wait32`.

Under `PTHREADS` every syscall is proxied synchronously to the main thread — `wrapSyscallFunction`
in `src/lib/libcore.js` ends with `library[x + '__proxy'] = 'sync'` for any syscall that does not opt
out, and `$proxyToMainThread` reaches `_emscripten_run_js_on_main_thread`
(`system/lib/pthread/proxying.c:622`), which posts to `emscripten_proxy_get_system_queue()` targeting
`emscripten_main_runtime_thread_id()`. So the futex the workers are waiting on is the proxy reply.

## The queue is empty

We added a probe: 90 s into a stall, the main thread calls
`emscripten_proxy_execute_queue(emscripten_proxy_get_system_queue())` itself and prints the progress
counter before and after. In **8 of 8** stalled shards it ran nothing and changed nothing
(`copies 292911 -> 292911`), and the program stayed wedged for the rest of the run. Neither early-out
in that function can have skipped the work silently: `system_queue_in_use` is `_Thread_local`, and
`tasks->processing` is only set inside `em_task_queue_execute`, which the live main loop is not
inside.

So the task is not sitting unexecuted in the queue. Either it already ran and the completion signal
never reached the worker, or it never got queued.

## What does NOT reproduce it

Every arm below ran on the same runner image, the same day, alongside a control leg that stalled as
usual. Exposure is counted in proxied syscalls, against the control's ~110 M per 12 shards.

| arm | shards | stalls | exposure |
| --- | --- | --- | --- |
| `emscripten_proxy_sync` of a no-op C function, 2 threads | 48 | 0 | 1.7 billion |
| proxied `open()` of a path that does not exist, 2 threads | 48 | 0 | 1.6 billion |
| proxied `open()` + `close()` of a real file, 2 threads | 24 | 0 | 700 million |
| proxied `write()` of one short line, 2 threads | 24 | 0 | 550 million |
| the whole copier loop, 2 threads | 24 | 0 | ~300 million |
| the whole copier loop, 1 thread | 24 | 0 | ~300 million |
| **the stalling program with the writer removed** | 24 | 0 | ~190 million |
| **the stalling program with the copier removed** | 24 | 0 | ~370 million |
| `-sMALLOC=emmalloc`, otherwise the stalling program | 24 | 4 | ~110 million |

The first four say the proxying path is not enough on its own: a bare `emscripten_proxy_sync` loop
survived 1.7 billion round trips, and so did 1.6 billion proxied syscalls that fail with `ENOENT`.
The last four are the interesting ones. Deleting either thread from the program that stalls makes
it stop stalling, at up to three times the exposure - and two copiers together, or two writers
together, are clean as well. **The hang appears to need two threads proxying different syscall
sequences concurrently**, not merely two threads proxying.

`-sMALLOC=emmalloc` does **not** help (4 of 24, the same rate as dlmalloc), which distinguishes this
from the malloc-lock/proxy-queue interaction in #24570.

Also ruled out earlier, each with >= 16 samples per arm: heap size (0 / 25 / 100 / 600 MB, and
`INITIAL_MEMORY=256MB` with no allocation); `ALLOW_MEMORY_GROWTH`; `PTHREAD_POOL_SIZE` 2 vs 3; stdio
versus raw file descriptors (`std::ofstream` and `std::filesystem::copy` hang just the same, and the
raw-fd variant takes no `FLOCK`, which is what distinguishes this from #20059); host architecture
(arm64 and x64); emsdk 4.0.10 and 4.0.19. A singlethreaded build of the same program has never
stalled in any configuration - it never proxies.

## Browser too

The same program in headless Firefox 153 (via `emrun`, `dom.maxHardwareConcurrency=2`) hangs at a
similar rate, and its main thread also keeps running - the `requestAnimationFrame` callback goes on
printing, and the hand drain there changes nothing either. Node reproduces it more often per shard
and can be attached to from outside, which is why the reproducer defaults to node.

## Questions

1. Is a synchronously proxied syscall expected to be able to block forever when the main thread is
   alive, idle and has an empty proxying queue?
2. Given the queue is empty at that point, is the completion signal on `ctx.sync.cond`
   (`proxying.c:399`) the more likely suspect, or can a task be lost before `em_task_queue_send`
   makes it visible?
3. Is there anything an application can do about it short of confining all file I/O to one thread?

Related: #24570 (proxying queue deadlock; its fix #24565 is already in 4.0.19 and we still hang) and
#20059 (stdio `FLOCK` against the proxying queue; our raw-fd variant takes no `FLOCK`).
