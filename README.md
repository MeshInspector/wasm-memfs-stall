# wasm-memfs-stall

A filesystem call on a pthread stops returning under emscripten, and the main thread stops with it.

Extracted from a real application, where a STEP import hung for the full timeout about once in
1200 runs while the page kept painting. Everything application-specific has been stripped: what is
left is ~110 lines with two threads and no dependencies.

## What it does

* one thread loops `std::filesystem::remove` + `std::filesystem::copy` on a 100 KB file
* one thread appends a line to a log file and flushes, in a loop
* `main()` installs `emscripten_set_main_loop`, so the main thread is genuinely back in the
  browser event loop between frames; the frame callback watches a counter and exits 3 if no
  copy finishes for 60 s

## Running it

```
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src emscripten/emsdk:4.0.19-arm64   em++ -std=c++20 -O2 -pthread memfs_repro.cpp -s PTHREAD_POOL_SIZE=navigator.hardwareConcurrency   -s PTHREAD_POOL_SIZE_STRICT=0 -s ALLOW_MEMORY_GROWTH=1 -s EXIT_RUNTIME=1 --emrun -o out/repro.html
python3 emrun.py --browser=/usr/bin/firefox --browser-args="--headless" --kill-exit out/repro.html
```

`.github/workflows/repro-memfs.yml` does both in CI, with a matrix over the knobs.

## Reading a run

| output | meaning |
|---|---|
| `STALLED: no copy finished for 60 s after N copies` | reproduced |
| `done: N copies in S s` | clean |
| neither, and the step times out | the severe variant, where the main loop itself stopped |

`Exiting due to channel error.` is only Firefox teardown noise from `--kill-exit`. A stalled run
burns its whole step timeout, because `emscripten_force_exit` cannot complete while a thread is
wedged -- so grep for the marker, not the exit code.

## Knobs

All compile-time defines: `BALLAST_MIB`, `TOUCH_BALLAST`, `RUN_SECONDS`, `SECOND_THREAD_FS`.
The pthread pool size is set at run time through the Firefox pref `dom.maxHardwareConcurrency`.

## Measured so far

| variant | stalled |
|---|---|
| stdio writer + `std::filesystem::copy` | 3/44 |
| raw `write()` writer + `std::filesystem::copy` | 1/32 |
| raw everywhere: `open`/`read`/`write`/`unlink` | 3/32 |
| ballast 0 / 25 / 100 / 600 MB | no difference detectable |
| pthread pool 2 vs 3 | no difference detectable |

Two threads touching MEMFS and a main thread in a real event loop are the only things
established. Everything else -- heap size, pool size, `std::filesystem::copy` specifically,
and stdio's FILE lock -- is refuted or unsupported. All three I/O variants stall, including the one that
takes no `FLOCK` anywhere, which distinguishes this from
[emscripten#20059](https://github.com/emscripten-core/emscripten/issues/20059).

A hung shard does not always print `STALLED`: in the severe form the frame callback stops
too, so the job simply runs until something kills it. **Judge a cell by its duration against
its cohort**, not by the marker alone -- a 16-minute job among 5-minute ones is a hang
whatever its conclusion says.

At a per-shard rate near 10%, a 12-sample arm cannot establish that an ingredient is
required: a clean 12/12 happens by chance about 28% of the time. Read these numbers as rates
to be pooled, not as presence/absence.

## What is known

Reproduces on emsdk 4.0.19, Firefox 153 headless, Ubuntu 24.04, 2 CPUs via `taskset`.
Roughly 1 shard in 3 within 8 minutes; one hit came after 15 copies, another after 103,942.

The application side also showed it in `compressZip`, in `decompressZip`, in a bare
`std::filesystem::copy`, and once before the loader's first log line -- i.e. at any filesystem
call, never in computation. The singlethreaded build of the same application has never stalled.

Ingredients still being measured, and **not** established: the pool size relative to the thread
count, and the heap size. Early single-arm results suggested both mattered, but 8 samples per arm
is far too few at this rate and a later grid contradicted them.
