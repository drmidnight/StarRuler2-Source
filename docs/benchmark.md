# Benchmark

How performance is measured for the work in
[metal-and-threading-plan.md](metal-and-threading-plan.md). Record a row in each results table
after every milestone. Compare only numbers taken on the same machine, with the same settings.

## Machine

| Field | Value |
|---|---|
| Model | Apple M4 Pro, 14 cores (from the game log: "Starting 15 threads on 14 processors") |
| macOS | |
| Display | 1800x1169 points, 3600x2338 pixels (2x) |

## Builds

| Build | Command | Binary |
|---|---|---|
| Release | `make -f source/linux/Makefile compile` | `bin/osx64/StarRuler2.bin` (+ `.dSYM`) |
| ThreadSanitizer | `make -f source/linux/Makefile compile SANITIZE=thread` | `bin_d-thread/osx64/StarRuler2.bin` |
| AddressSanitizer + UBSan | `make -f source/linux/Makefile compile SANITIZE=address` | `bin_d-address/osx64/StarRuler2.bin` |

Run sanitizer binaries from the repository root. Write reports to files, for example:
`TSAN_OPTIONS="halt_on_error=0 log_path=/tmp/sr2-tsan" ./bin_d-thread/osx64/StarRuler2.bin`

## Headless smoke test

Compiles every script without a window, then exits. It needs no setup, so it runs on every change:

    ./bin/osx64/StarRuler2.bin --test-scripts --no-window --no-sound

Exit code 0 is a pass. Without `--no-window` the game stays open on the console screen instead
of exiting.

## In-game benchmark

1. **Save:** a late-game save with a large galaxy and several AI empires. Keep it at
   `~/.starruler2/saves/benchmark.sr2` (not in the repository). Record what it contains:

   | Field | Value |
   |---|---|
   | Galaxy | |
   | Systems | |
   | Empires (AI difficulty) | |
   | Game time | |
   | Approximate ship count | |

2. **Settings:** default graphics settings, window mode as in the Machine table, vsync off,
   frame cap off.
3. **Camera:** load the save, press Home to centre on the capital, and zoom out to show the whole
   empire. Note anything different here.
4. **Runs:**
   - **Paused, 60 s.** Measures render and animation cost.
   - **Unpaused at normal speed, 60 s.** Measures simulation and script cost.
   - **Unpaused at maximum speed, 60 s.** Measures simulation throughput: game seconds advanced
     per wall second.
   - **Main menu idle, 60 s.** Measures idle CPU.

### Capturing

- **Frame times and phases:** run Instruments with the Time Profiler and os_signpost (Points of
  Interest) instruments. The engine marks these intervals:

  | Signpost | Where | Covers |
  |---|---|---|
  | Tick global | `main/tick.cpp` | Script GC and queuing the script ticks, including the wait for GC |
  | Script tick | `main/tick.cpp` | One script engine's tick (labelled Server, Client or Menu) |
  | Script GC | `main/tick.cpp` | One script engine's garbage collection (labelled; "(full)" for full cycles) |
  | Animate | `main/tick.cpp` | Scene animation for a frame, including waiting on script ticks |
  | Render | `main/tick.cpp` | Rendering a frame |
  | Lock globals | `obj/lock.cpp` | The global simulation step between tick cycles |
  | Save game | `main/save_load.cpp` | Saving, including autosaves |
  | Texture upload | `resource/load_material.cpp` | One texture upload step |

  From the command line:

      xcrun xctrace record --template 'Time Profiler' --instrument os_signpost \
          --time-limit 60s --output /tmp/sr2.trace --launch -- ./bin/osx64/StarRuler2.bin

- **Thread behaviour:** run Instruments with the System Trace template. Look for threads that wake
  every millisecond, and for runnable time spent in spin loops.
- **CPU:** `top -pid <pid> -l 60 -s 1 -stats cpu` and average the samples, or read the CPU column
  from Activity Monitor.
- **Energy:** `sudo powermetrics --samplers cpu_power -i 1000 -n 60` and average the combined
  power.
- **Startup:** time from launch to the main menu. The log prints the script load and compile
  times, for example `Server scripts: 57ms load, 682ms compile`.

## Results

### Headless smoke test (`--test-scripts --no-window --no-sound`)

| Date | Build | Wall time | CPU (user + sys) | Peak memory | Script compile (shadow / client / server) | Notes |
|---|---|---|---|---|---|---|
| 2026-10-23 | M0, C++20 | 1.3 s | 3.3 s + 2.2 s | 228 MB | 289 / 581 / 678 ms | About 1 run in 60 exits with heap corruption during startup (2 in 120 on this branch, 1 in 110 on `master`): see M1 item 8 |

### Idle CPU

| Date | Build | Screen | Wall time | CPU time | Average CPU | Notes |
|---|---|---|---|---|---|---|
| 2026-10-23 | M0, C++20 | Console screen, windowed, nothing happening | 109 s | 108 s (83 user + 25 sys) | ~100% (one full core) | 1.3 million involuntary context switches. The worker threads poll with 1 ms sleeps. |

### In-game benchmark

| Date | Build | Paused frame time (median / p99) | Unpaused frame time (median / p99) | Max-speed sim rate (game s / wall s) | Menu idle CPU | Package power | Startup to menu |
|---|---|---|---|---|---|---|---|
| 2026-10-23 | M0 baseline (normal speed, vsync on, 120 fps cap: not the standard settings) | not measured | 8.4 / 21.4 ms (105 fps mean; capped) | not measured | | | |

### Late-game profile, M0 baseline (2026-10-23)

Taken on the `benchmark` save at normal speed, unpaused, with vsync on and a 120 fps cap. Signpost
numbers come from a 30 s `xctrace` recording; per-thread CPU numbers come from a 10 s `sample`.

| Signpost | Per second | Median | p95 | p99 | Max | Share of one core |
|---|---|---|---|---|---|---|
| Render (includes the frame limiter and swap) | 105 | 7.75 ms | 15.7 ms | 20.6 ms | 26.6 ms | 93% |
| Lock globals | 105 | 1.27 ms | 2.54 ms | 3.68 ms | 5.09 ms | 19% |
| Animate | 105 | 0.55 ms | 0.91 ms | 1.15 ms | 6.24 ms | 6% |
| Script tick, Client | 105 | 0.22 ms | 0.41 ms | 0.51 ms | 6.22 ms | 3% |
| Script tick, Server | 105 | 0.08 ms | 0.14 ms | 0.19 ms | 0.33 ms | 1% |
| Script GC, Client and Menu | 105 each | 0.02 ms | 0.04 ms | 0.06 ms | 0.11 ms | <1% |

Frame-to-frame time: median 8.4 ms, p95 16.3 ms, p99 21.4 ms, max 55.7 ms.

| Thread group | Threads | Cores on CPU | Notes |
|---|---|---|---|
| Simulation workers (`processing::process`) | 15 | 3.9 | 3.6 cores in `Object::think`, 3.1 of it in script component calls through `handleGeneric`/`wrapperFunc`; 0.8 in `findTargets`. 59% of their wall time is spent asleep in `threads::idle()`. |
| Empire script threads (`runScriptThread`) | 13 | 1.6 | Through `emp_wrapperFunc`/`handleGeneric`. |
| Empire message threads | 14 | 1.0 | `AsyncEmpireScriptCall::process`. |
| Message threads (`messageProcess`) | 15 | 0.7 | Spinning on `ReadWriteMutex::readLock`/`try_lock` in `tickRandomMessages`. |
| Animation threads | 13 | 0.4 | 97% asleep in a 1 ms poll loop. |
| Main thread | 1 | 0.3 | 42% in the frame limiter's sleep, 18% waiting on read locks in `tickLockMessages`. |
| Total | 80 | 7.9 | |
