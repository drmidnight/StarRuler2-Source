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
  | Lock group | `obj/lock.cpp` | One worker's slice of object ticks and messages |
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
| | M0 baseline | | | | | | |
