# The wasm STEP-import CI stall: an emscripten bug, fixed upstream in 5.0.5

**Nothing to file. The bug is emscripten's, it was fixed on 2026-04-01 in
[#26582](https://github.com/emscripten-core/emscripten/pull/26582), and the fix shipped in emsdk
5.0.5 on 2026-04-03. MeshInspectorCode pins `emscripten/emsdk:4.0.19` in
`docker/emscriptenDockerfile:24`, which predates it. Bumping that should end the flake.**

## The bug

`emscripten_proxy_finish` signalled the proxy context's condition variable *after* releasing its
mutex:

```c
pthread_mutex_lock(&ctx->sync.mutex);
ctx->sync.state = DONE;
remove_active_ctx(ctx);
pthread_mutex_unlock(&ctx->sync.mutex);
/* preempted here */
pthread_cond_signal(&ctx->sync.cond);
```

In that window the waiting thread can take the mutex, see `DONE`, skip `pthread_cond_wait`
entirely, return from `emscripten_proxy_sync_with_ctx` and destroy the context — which lives on its
own stack. The finishing thread then signals a condvar that no longer exists. The fix is one line of
reordering: signal while still holding the mutex.

This matters to us because **under `-pthread` emscripten proxies every syscall synchronously to the
main thread** — `wrapSyscallFunction` in `src/lib/libcore.js` assigns `__proxy: 'sync'` to any
syscall that does not opt out, and `$proxyToMainThread` routes through
`emscripten_proxy_get_system_queue()`. So every file operation on a worker thread rode this race.
That is why the app hung at arbitrary points of the import — unzip, `std::filesystem::copy`,
`compressZip`, the scene load — and why the stage moved between occurrences: the stage was never
the point.

## How we got there

**The stacks.** node reproduces it more often than the browser (7/48 vs 1/20) and can be attached
to, which the browser could not be — `MOZ_PROFILER_SHUTDOWN` only writes on a clean shutdown, which
a wedged page prevents. `gdb -p <node> -batch -ex "thread apply all bt"` on a wedged process, five
captures, all alike:

```
Thread 1 "node": __GI_epoll_pwait () / uv__io_poll () / uv_run ()      <- idle, nothing queued
Thread 2 "node": Builtins_WasmCEntry () / Runtime_WasmI32AtomicWait ()
                 / v8::internal::FutexEmulation::WaitWasm32 ()
Thread 3 "node": (identical)
```

Both workers parked in `memory.atomic.wait32`; the main thread alive and idle. 9 of 10 worker
samples looked like this.

**The queue is empty.** A probe that made the main thread call
`emscripten_proxy_execute_queue(emscripten_proxy_get_system_queue())` 90 s into a stall ran nothing
and changed nothing in 8 of 8 stalled shards (`copies 292911 -> 292911`), browser included. The work
had already been done — consistent with a lost completion signal, not a lost notification.

**The ablation grid**, each arm run beside a control leg that stalled as usual:

| arm | shards | stalls | exposure in proxied syscalls |
| --- | --- | --- | --- |
| `emscripten_proxy_sync` of a no-op C function, 2 threads | 48 | 0 | 1.7 billion |
| proxied `open()` of a missing path, 2 threads | 48 | 0 | 1.6 billion |
| proxied `open()`+`close()` of a real file, 2 threads | 24 | 0 | 700 million |
| proxied `write()` of one short line, 2 threads | 24 | 0 | 550 million |
| the whole copier loop, 2 threads / 1 thread | 24 / 24 | 0 / 0 | ~300 million each |
| the real program, writer deleted / copier deleted | 24 / 24 | 0 / 0 | 190M / 370M |
| the real program with `-sMALLOC=emmalloc` | 24 | 4 | ~110 million |
| **the real program (copier + writer)** | 12 | **2** | ~110 million |

Deleting either thread stops it; making both threads do the *same* thing stops it; only a copier and
a writer together, with differently-timed proxied calls, opens the preemption window the race needs.
`-sMALLOC=emmalloc` does not help, which rules out the malloc/proxy interaction of
[#24570](https://github.com/emscripten-core/emscripten/issues/24570) (whose own fix, #24565, is
already in 4.0.19).

**The version bracket** confirms the boundary, 24 shards per cell unless noted:

| emsdk | 4.0.19 | 4.0.23 | 5.0.0 | 5.0.4 | 5.0.5 | 5.0.7 | 6.0.0 | 6.0.9 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| stalls | ~15% | 3 | 5 | 3 | **0** | **0** | **0** | **0** (72) |

Zero stalls in 144 shards on 5.0.5 and later, against ~15% per shard below it.

## What to do

1. Bump `docker/emscriptenDockerfile:24` from `emscripten/emsdk:4.0.19` to 6.0.9, or at minimum
   5.0.5. Worth checking the toolchain image and `CLANG_GCC_PIN` move with it.
2. Close MIC#7724 against this, noting the real rate was ~1 in 1200 imports, not 1 in 350.
3. The reproducer stays at https://github.com/MeshInspector/wasm-memfs-stall — it is the regression
   test if this ever comes back.

## Ruled out along the way

The AWS converter (CloudWatch showed `/request` answered in 96 ms, then 10m15s of client silence);
the archive data; libzip; heap size and `ALLOW_MEMORY_GROWTH`; `PTHREAD_POOL_SIZE`; stdio versus raw
file descriptors (which is what distinguishes this from
[#20059](https://github.com/emscripten-core/emscripten/issues/20059) — the raw-fd variant takes no
`FLOCK` and hung just the same); `std::filesystem::copy` specifically; host architecture; and our own
instrumentation. A singlethreaded build never stalled in any configuration, because it never proxies.
