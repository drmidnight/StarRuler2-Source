# Metal Backend and Threading Overhaul Plan

Status: 2026-10-27. M0 to M4 are merged to `master` (M3 partly: M3.1, M3.2, M3.4, M3.5 phase 1 and
M3.7 are done; the M4 window-mode test matrix is partly run). M5 is in progress on `m5-renderer`:
M5.1 to M5.5 are done; M5.6 is next. iOS is out of scope for now; see [iOS readiness](#ios-readiness).
Measurements are in [benchmark.md](benchmark.md).

This plan moves the Apple Silicon fork from OpenGL 2.1 to a native Metal renderer and replaces the
engine's spin-and-sleep threading with blocking, QoS-aware primitives. The fork is Apple-only
(arm64, macOS 13+), so portability layers such as ANGLE, bgfx or MoltenVK are out of scope.

## Decisions

| Topic | Decision |
|---|---|
| Platform | Apple Silicon only, macOS 13+ deployment target |
| Language | Game code moves to C++20. Vendored libraries keep their own settings. |
| Window/input layer | Upgrade the in-tree GLFW 3.0.2 to GLFW 3.4, built through `source/macos/build_deps.sh`. SDL3 stays a later option. |
| Shaders | Translate the existing GLSL at runtime: a built-in-rewriting shim, then glslang (SPIR-V), then SPIRV-Cross (MSL). This keeps mod shaders working. Hand-written MSL for hot shaders comes later. |
| Metal API binding | Objective-C++ (`.mm`) with ARC, confined to the Metal backend directory |

## Goals

1. Stop burning CPU on idle threads, and stop P/E-core priority inversion.
2. Remove data races that depend on x86 memory ordering.
3. Run the renderer on Metal with Retina support, proper frame pacing and background texture creation.
4. Keep the game playable on `master` after every milestone.
5. Keep mods that ship GLSL shaders working.

Not in scope: an arm64 JIT for AngelScript, gameplay-script optimisations, network protocol changes.
These are covered in the wider performance audit.

## Success measures

Capture before starting and after each milestone, all on one fixed benchmark:

- **Benchmark setup:** a late-game save with a large galaxy and several AIs. Record the camera and
  settings in `docs/benchmark.md`.
- **Frame time:** median and 99th-percentile frame time over 60 s, both paused and unpaused.
- **Simulation throughput:** game seconds simulated per wall second at max speed.
- **CPU:** total CPU % in the main menu and at a paused game. The target is near zero when idle.
- **Energy:** package power from `powermetrics` over 60 s of normal play.
- **Startup:** time from launch to the main menu, cold and warm.
- **Spinning:** the System Trace in Instruments should show threads blocking, not spinning or
  waking every millisecond.

---

## Milestone overview

```
M0 Baseline & tooling ──┬──> M1 Correctness fixes ──> M2 Threading primitives ──> M3 Scheduling
                        │
                        └──> M4 GLFW 3.4 ──> M5 Metal-shaped GL ──> M6 Metal backend ──> M7 Metal-only wins, remove GL
```

The threading track (M1 to M3) and the rendering track (M4 to M7) are independent until M6. M6
wants M3's frame pacing and worker pool, but can start without them. One person can interleave the
two tracks. Two people can take one each.

---

## M0: Baseline and tooling

### M0.1 Switch the game code to C++20
- `source/linux/Makefile:98`: change `-std=c++11` to `-std=c++20`. Check each sub-Makefile under
  `source/*/gcc/` for its own `-std` flag. AngelScript (`source/linux/build.sh`) keeps its current
  setting.
- Known fixes:
  - `std::binary_function` was removed in C++17. Change `caseless_less` in
    `source/game/scripts/bind_irc.cpp:47` to a plain struct.
  - `volatile` compound assignment is deprecated in C++20. `atomic_int` (`os/include/threads.h:43-45`)
    is rewritten in M2 anyway.
  - C++20 rewrites `operator==`, and that can make some overloads ambiguous. There are 14
    hand-written `operator==` definitions in `game/` and `util/`. Fix whatever the compiler flags.
- Build with `-Wall` and fix new warnings before moving on.

### M0.2 Build variants
Add Makefile switches next to the existing `DEBUG`/`ADD_DBG`/`NLTO`:
- `TSAN=1`: `-fsanitize=thread -g -O1`, no LTO. This is only useful after M2, because TSan does not
  understand the hand-rolled spinlocks.
- `ASAN=1`: `-fsanitize=address,undefined -g -O1`, no LTO.
- Keep a dSYM for release builds: `-g` then `dsymutil`, stored next to the disk image and not
  shipped. This lets us symbolicate crash logs that users attach.

### M0.3 Instrumentation
- Add `os_signpost` intervals, behind a tiny `prof::Interval` RAII wrapper, around:
  - `tickGlobal`
  - each `RunScriptTick` and `RunScriptGC`
  - the animation phase and render phase in `renderFrame`
  - `LockGroup::process`
  - `updateLockGlobals`
  - `saveGame`
  - texture uploads

  These show up in Instruments' Points of Interest track, cost almost nothing, and replace the ad-hoc
  `PROFILE_*` defines over time.
- Record the M0 baseline numbers in `docs/benchmark.md`.

**Exit:** the game builds and runs as C++20, the baseline is recorded, and TSan/ASan builds link.

---

## M1: Correctness fixes (small, ship first)

| # | Fix | Location |
|---|---|---|
| 1 | Add the missing braces around the fence + `retValue->asyncReturned = true`. Today the assignment runs even when `retValue` is null. | `source/game/scripts/script_components.cpp:1061-1063` |
| 2 | `pauseScripts` and `clearScripts` become `std::atomic<bool>`, using seq_cst on both sides of the pause handshake with `scriptThreadsActive`, which also becomes atomic. This removes a Dekker-style race that can let GC run while a script thread enters. | `source/game/scripts/manager.h:202-203`, `manager.cpp:1562-1590` |
| 3 | Cross-thread flags become `std::atomic<bool>`, with release on store and acquire on load: `stopProcessing`, `pauseProcessing`, `EndAnimationThreads`, the `Projectiles*` flags, `asyncReturned`, and the loader's `done`/`finished`/`finalized`. Delete the standalone fences that commit 6d37b0d added, because a fence only synchronises when it is paired with an atomic access. | `processing.cpp:22`, `tick.cpp:193`, `projectiles.cpp:606-609`, `generic_call.h:61`, `initialization.cpp:1490`, `util/threaded_loader.cpp` |
| 4 | Queue-emptiness checks made without the lock: `process_queue.empty()`, `lockQueue.empty()` and `lock->messages.empty()`. Keep an atomic count next to each queue and read that instead. | `processing.cpp:63`, `lock.cpp:316,364` |
| 5 | `PhysicsGrid::updateItem` writes `item->bound` while holding only the read lock. **Deferred to M3:** taking the write lock would serialize every object move on the spinning RW lock. The race only gives spatial queries a stale or torn bounding box. Fix it with double-buffered bounds in the M3 scheduler. | `physics/physics_world.cpp:368-386` |
| 6 | Vision donation does `other->donatedVision |= mask` across lock groups without an atomic. Use `std::atomic_ref<uint32_t>` (C++20) with `fetch_or`. | `obj/object.cpp:322-334` |
| 7 | The thread count for lock groups and the thread count started disagree: `threadCount=cpu+1`, but `start(cpuCount)` is called. Make them one value. This gets superseded in M3. | `main/initialization.cpp:1430-1431` |
| 8 | **Startup heap corruption (found in M0).** About 1 headless startup in 60 crashes in `malloc` with a corrupted free list, on both `master` and C++20 builds. A TSan run of `--test-scripts` reports 26 races on the startup path. They are in the threaded loader (`Loading::tasks` and the `finished`/`finalized` flags, `util/threaded_loader.cpp:86-174`), and in shared tables written while server, shadow and client bindings register in parallel (`scripts::SetObjectTypeOffsets` at `bind_dynamic.cpp:69,91`, `addComponentStateValueTypes` at `script_components.cpp:339`, the particle-system map, `loadClientScripts` at `initialization.cpp:509`). Fix the loader with a mutex and condition variable. Make shared registration either run once before the parallel loads, or take a lock. Re-run TSan until the startup path is clean. | `util/threaded_loader.cpp`, `scripts/bind_dynamic.cpp`, `scripts/script_components.cpp`, `main/initialization.cpp` |
| 9 | `--no-window` runs crashed in `glGetError` with no GL context. Fixed in M0, so headless smoke tests work. | `main.cpp` |
| 10 | **Misaligned script value types (found in M0 with UBSan).** AngelScript 2.37 places every value type at 4-byte alignment. `asOBJ_APP_CLASS_ALIGN8` only changes how arm64 native calls pass the type, and `asOBJ_APP_ALIGN16` needs the unfinished `WIP_16BYTE_ALIGN` build option. Plain loads and stores tolerate this on Apple Silicon. Only atomics fault, if misaligned across a 16-byte boundary. The one exposed type is `locked_double` (8-byte compare-and-swap) as a script class member or local, which no game script uses (both script uses are `locked_int`, safe at 4 bytes). **Done:** fixed `LocklessType::set` returning the old value's raw bits and `interp` offsetting from the target. **Open:** if mods need `locked_double` in classes, make its operations safe at 4-byte alignment. | `util/lockless_type.h`, `scripts/bind_data.cpp` |

**Exit:** each fix is its own commit, and the game plays a full benchmark session without regressions.

---

## M2: Threading primitives (same API, new implementation)

**Done, with these differences from the plan below:**
- `atomic_int` uses sequentially consistent operations, not acquire/release. On arm64 they cost the
  same, and the engine has Dekker-style handshakes that need the full ordering.
- `ReadWriteMutex` uses `pthread_rwlock_t`. Darwin's rwlock takes read locks with a single atomic
  operation, while libc++'s `std::shared_mutex` takes an internal mutex for every read lock.
- `wait_compare_exchange` was unused and has been removed.
- Making `Mutex` non-copyable found a bug: `getQueriedServers` copied the query mutex instead of
  locking it.
- `Empire::statLocks` is a `std::deque`, so growing it never moves a mutex.
- Run the tests with `make -f source/linux/Makefile test_threads`, and add `SANITIZE=thread` for the
  TSan build.

The `threads::` API is used widely, so we keep it and swap the implementations. Approximate use
counts:

| Primitive | Uses |
|---|---|
| `Mutex` | about 86 |
| `ReadWriteMutex` | about 49 |
| `Signal` | about 17 |
| `atomic_int` | about 95 |
| sleep/idle calls | about 95 |

The files are `source/os/include/threads.h`, `source/os/source/threads.cpp` and
`source/os/source/threads_gcc.cpp`.

### Behaviour that must be preserved
- **`Mutex`:**
  - It is recursive: it tracks the owning thread and a lock count.
  - `hasLock()` reports whether the calling thread owns it.
  - `try_lock()` exists.
- **`ReadWriteMutex`:**
  - Read locks are recursive. Write locks are recursive.
  - The thread holding the write lock may also take read locks.
  - Upgrading a read lock to a write lock throws.
  - Each thread tracks its holdings in a thread-local table of 8 entries.
- **`Signal`:** an integer that other threads wait on until it equals, or stops equaling, a value.
  It supports `signal`, `signalUp`/`signalDown` (by one or by n), `check`, `checkAndSignal`, `wait`,
  `waitNot` and `waitAndSignal`.
- **`atomic_int`:** loads are acquire. Today every operation is an out-of-line call
  (`threads_gcc.cpp:78-132`).

### M2.1 `atomic_int` and the free functions
- Re-implement `atomic_int` inline in the header over `std::atomic<int>`:
  - loads: acquire
  - stores and `exchange`: release
  - read-modify-write operations: acq_rel
  - `compare_exchange_strong` keeps its current argument order: `(value, compareTo)`, returning the
    previous value.
- `wait_compare_exchange` spins up to about 64 times with `__builtin_arm_yield()`, then calls
  `value.wait(observed)`. Every store and read-modify-write that could release a waiter calls
  `notify_all()`.
  - The cost: `notify_all` is a syscall only when there are waiters, because libc++ on Apple uses
    `__ulock_wake`. Confirm this with a microbenchmark, and if it isn't free, add a waiter counter.
- `swap` and `compare_and_swap` (`threads.h`) become inline `__atomic` builtins.

### M2.2 `Mutex`
- Back it with `os_unfair_lock`, plus `std::atomic<int> owningThread` and `unsigned lockCount` for
  recursion and `hasLock()`.
- `os_unfair_lock` parks waiters in the kernel and donates priority to the owner. That fixes the
  inversion where waiters on P-cores spin while the owner sits on an E-core.
- It aborts if the unlocking thread is not the owner. That is a feature here: it surfaces any hidden
  cross-thread release.
  - **Audit step:** before landing, run the benchmark and a multiplayer session. If anything traps,
    that call site gets its own primitive (a `Signal` or a binary semaphore).
- Add `Mutex(const Mutex&) = delete`. An `os_unfair_lock` must not be copied or moved, so the compiler
  will find any copies.
- Keep `PROFILE_LOCKS` compiling, using a contention counter on the slow path only.

### M2.3 `ReadWriteMutex`
- Back it with `std::shared_mutex`. libc++'s version prefers writers, so writers can't be starved.
- Keep the thread-local table, so the underlying lock is taken only once per thread. Each entry
  becomes `{lock, count, mode}`.
  - `readLock`:
    - If this thread owns the write lock, or already holds a read lock, increment the count.
    - Otherwise call `lock_shared()`.
  - `writeLock`:
    - If this thread already owns the write lock, increment the count.
    - If it holds read locks, throw, as today.
    - Otherwise call `lock()`, set `owningThread`, and record mode=write.
  - `release`: decrement. On reaching 0, unlock in the recorded mode and clear `owningThread` for
    writers.
- Raise the table size from 8 to 16 and keep the assert.
- Readers no longer queue one by one: today every `readLock` briefly takes `owningThread`
  exclusively.
- Benchmark `pthread_rwlock_t` as an alternative backend. Pick whichever wins on `LockGroup` and
  physics workloads.

### M2.4 `Signal`
- Back it with `std::atomic<int>`. Every mutator ends with `flag.notify_all()`.
- `wait(v)`: spin about 32 times with yield, then loop while `cur != v` calling `flag.wait(cur)`.
- `waitNot`: the mirror of `wait`.
- `waitAndSignal`: a CAS loop that waits on failure.

### M2.5 Sleep, idle and thread creation
- `threads::idle()` stops sleeping 1 ms on Apple. M3 removes its callers. Until then, change
  `processing.cpp:353` to idle only after an iteration that did no work.
- Add `threads::sleepUntil(time)` using `clock_nanosleep`-equivalent `mach_wait_until`.
- Implement `setThreadPriority` (`threads_gcc.cpp:32`) with `pthread_set_qos_class_self_np`:

  | Priority | QoS class | Used by |
  |---|---|---|
  | `TP_High` | `QOS_CLASS_USER_INTERACTIVE` | simulation, animation |
  | `TP_Normal` | `QOS_CLASS_USER_INITIATED` | loaders |
  | `TP_Low` | `QOS_CLASS_UTILITY` | autosave, network, sound streaming |

- `createThread` gets an optional name, set with `pthread_setname_np`, so threads are labelled in
  Instruments and crash logs.
- `getNumberOfProcessors()` keeps its meaning (all cores). Add `getPerformanceCores()`, which reads
  `sysctlbyname("hw.perflevel0.logicalcpu")` and falls back to `hw.activecpu`.

### M2.6 Clocks
- Replace `gettimeofday` in `getTime`/`getAccurateTime` (`os/glfw_driver.cpp:409-440`) with
  `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`, which is monotonic and doesn't jump with wall-clock
  changes.
- Keep game-time accumulation the same.

### M2.7 Verification
- Unit tests in a new `source/os/test/` covering:
  - recursion
  - read-under-write
  - upgrade-throws
  - `Signal` wait/notify under contention (8 to 16 threads, randomised sleeps)
  - `try_lock` semantics
- Run a TSan build of the game through the benchmark. File every report: fix it now, or list it in
  `docs/tsan-known.md` with a reason.

**Exit:** primitives replaced, tests passing, TSan clean or triaged, idle CPU much lower (expected,
measure it). No behaviour change in gameplay.

---

## M3: Event-driven scheduling

**Partly done, with these differences from the plan below:**
- The pool (`os/work_pool.h`) has priority lanes: frame, action, tick. One worker per performance
  core.
- Messages run on a second pool of cpu+1 threads. An async script call with a return value blocks
  its job until another group handles the message, so messages sharing the main pool could deadlock
  once every worker is waiting.
- Lock groups tick once per frame instead of on a ready queue. Game time only advances in
  `swapBuffers`, so ticking more often did no work. A cycle that overruns a frame starts the next one
  right away.
- `Action::run` returns `AR_Done`, `AR_Again` or `AR_NextFrame`, so projectiles stop re-queuing in a
  busy loop.
- Empire threads block on a wake counter instead of polling.
- Benchmark save: 1.94 cores at normal speed (M2: 2.03), mostly rendering on the main thread. Paused,
  the workers use about 2% each. At the fastest speed the pool saturates all cores, and the physics
  rwlock is about 15% of the busy samples.

**M3.7 done on `m4-glfw`:** `swapBuffers` waits out the frame limit in `glfwWaitEventsTimeout`
instead of spinning, and `handleEvents` waits the same way. Paused on the benchmark save, the main
thread went from about 0.8 cores to 0.25.

**Still to do:** M3.3, M3.6, M1 item 5 and the
`PhysicsGrid::updateItem` rwlock contention, and the non-atomic `received` flag in
`bind_object.cpp` (`GetObjNode`/`GetObjName`). The in-game save/quit/new-game pass has not been run.

### Current structure (for reference)
- **`processing::start`** (`processing.cpp:85-92`) starts three pools:
  - cpu+1 "process" workers, each looping on `tickRandomMessages` → `tickRandomLock` → `idle()`;
  - cpu+1 message threads;
  - one empire-message thread per empire (`processing.cpp:280`).
- **Lock groups:** workers pick a random `LockGroup` from a global queue and tick up to 300 objects
  (`lock.cpp:363`). After all groups drain, whichever worker gets there first runs the global step
  `updateLockGlobals` (`lock.cpp:170`). That step handles switched objects, universe add/remove,
  `updateTargets` every 0.2 s, server GC, and delayed releases.
- **Animation:** the main thread animates scene nodes and drains `processing::run()` while it waits
  for script ticks. It also has cpu−1 animation threads that poll `animIndex` with `sleep(1)`
  (`tick.cpp:206-211`, `571`).
- **Empires:** each empire runs an empire tick thread at 20 Hz (`scripts/server/empire.as:424`
  drives it).

### M3.1 Worker pool
New `source/game/sched/` (or `source/os/` if it stays engine-agnostic):

- **Size:** N workers, N = `getPerformanceCores()`, a setting with that default. QoS
  USER_INTERACTIVE, named `sr2-worker-k`.
- **Two lanes:**
  - **frame:** animation and anything the current frame waits on;
  - **sim:** lock groups, messages, script ticks, GC.

  Workers always drain the frame lane first. That way a frame never waits behind a long simulation
  job, without needing a second pool that would oversubscribe the cores.
- **Idle waiting:** a `std::mutex` + `std::condition_variable` with `wait_until(nextTimedJob)`, so
  timed work (the next lock-group tick) wakes the pool without polling. Pushing work calls
  `notify_one`.
- **`helpUntil(pred)`:** the main thread's waiting primitive. It runs frame-lane, then sim-lane, jobs
  while `pred` is false, and parks on the pool condvar when there is nothing to run.
  - It replaces the `while(!signal.check(0)) { processing::run(); sleep(0); }` loops in `tickGlobal`
    and `renderFrame` (`tick.cpp:~455-470`, `614-660`).
  - It preserves today's behaviour of the main thread helping with work while it waits.
- **`processing::queueAction`:** keeps its signature and pushes to the sim lane. Change `popAction`
  from LIFO to FIFO, and stop re-queuing failed actions immediately. Instead, a failed action goes
  back with a short backoff or waits on the resource it needs.

### M3.2 Lock groups and messages
- **Replace the random pick with a ready queue.**
  - A group becomes ready when it has pending messages, or when its earliest object tick time has
    arrived.
  - A worker pops a ready group, processes messages and then due objects (still capped per slice,
    keeping 300 as the default), and recomputes the group's next due time.
  - The group then goes back on the ready queue if it has more work, or onto a timer heap if it
    doesn't.
- **Global step as an explicit phase barrier.**
  - An atomic counter of groups in flight replaces "first worker to notice".
  - When the counter hits 0 at a cycle boundary, one worker runs `updateLockGlobals`.
  - Split `updateTargets` into chunked jobs on the sim lane, and wait on them with `helpUntil`.
- **`removeTargetsTo`:**
  - Today it is O(removed × N) (`universe.cpp:18-24, 97-127`).
  - Collect the removed set first, then do one pass over children in chunks.
  - Optional follow-up: keep a reverse "targeted-by" list.
- **Message threads:** fold them into the pool. Posting a message to an idle group makes it ready.
- **Remove dead code:** the `LockGroup::process` sleep every 1024 ticks (`lock.cpp:719-723`), and
  `tickRandomMessages`/`tickRandomLock` once unused.

### M3.3 Message memory
`AllocOnlyRegion` (`memory/AllocOnlyPool.h:75-99`) only resets once every allocation in it is freed,
which never happens under load. It also doesn't align allocations.
- Replace it with per-thread free lists by size class, aligned to 16 bytes.
- Pool `DeferredObjMessage` (`object.cpp:246`) the same way.

### M3.4 Animation
- Delete the dedicated animation threads, `animIndex` polling and `EndAnimationThreads`.
- Each frame:
  1. Push `ceil(nodeCount/128)` chunk jobs to the frame lane.
  2. The main thread calls `helpUntil(animDone)`, and every finished chunk decrements a counter.
  3. `furthestNode` becomes a per-chunk max, reduced after the barrier.
  4. `sortChildren()` runs after the barrier.

  This fixes the current handshake race (`tick.cpp:633-636`).

### M3.5 Empire threads
- **Phase 1:** keep one thread per empire, but block instead of polling: a condvar or `sleepUntil`
  the next 20 Hz slot, and a blocking wait on the empire message queue. Name the threads. QoS
  USER_INITIATED.
- **Phase 2 (measure first):** move empire ticks onto the sim lane. Risk: empire scripts block inside
  ticks (async calls with return values, locks), and blocking inside a pool job can starve the pool
  or deadlock. Only do this after:
  - removing the 2500-iteration spin on async return values (`script_components.cpp:684-689`), and
  - giving async script calls a continuation or a "help while waiting" path.

### M3.6 Other threads and loops

| Loop | Fix |
|---|---|
| Threaded loader (`util/threaded_loader.cpp:115-118, 165`) | Use a GCD dispatch group on a USER_INITIATED queue, or a condvar. |
| Startup PNG decode (`initialization.cpp:571-579, 635-638, 1218-1220`) | `dispatch_apply` over pending images. `processImages` is already lock-protected (`load_material.cpp:153-220`). |
| Network (`network/source/message_handler.cpp:13-40`) | Block in `kqueue` (or `select`) with a timeout equal to the next resend/heartbeat deadline. Remove `sleep(1)`. |
| Sound streaming (`sound/source/CSoundDevice.cpp:75-100`) | Keep the 50 ms cadence for now, as a UTILITY-QoS thread with `sleepUntil`. |
| Autosave (`tick.cpp:827-829`) | Serialise to memory while paused, then resume and compress/write on a UTILITY GCD queue. Add `F_FULLFSYNC` before the rename. |
| libcurl (`scripts/bind_web.cpp`) | Set `CURLOPT_TIMEOUT`/`CONNECTTIMEOUT`. `~WebData` must not spin on the main thread. |
| IRC disconnect (`scripts/bind_irc.cpp:696`) | Join with a timeout instead of a `sleep(1)` poll. |

### M3.7 Frame pacing (before Metal)
- `swapBuffers` (`os/glfw_driver.cpp:120-133`) spins on `glfwPollEvents` + `sleep(0)`. Replace that
  with:
  - `sleepUntil(deadline)` then `glfwPollEvents`, when a frame cap is set;
  - `glfwSwapInterval(1)` when vsync is on.
- M6 replaces this with `CAMetalLayer` pacing.

**Exit:** System Trace shows no thread waking on a 1 ms cadence. The menu and a paused game use
near-zero CPU. Simulation throughput is equal or better. TSan is clean or triaged.

---

## M4: GLFW 3.4 upgrade

**Partly done, with these differences from the plan below:**
- GLFW 3.4 builds from `build_deps.sh`, pinned by SHA-256, and `source/glfw/` is deleted. Without
  `deps/macos`, the Makefile uses Homebrew's `glfw`.
- Fixed upstream: the key table swap, the monitor name crash, and the cursor warp freeze.
- `os/cocoa_window.mm` holds all native window code, so GLFW stays unpatched:
  - precise scrolling and pinch to zoom come from `NSEvent` local monitors;
  - borderless fullscreen at the desktop size stays. GLFW 3.4 picks the closest listed video mode, and
    macOS doesn't list scaled Retina desktop modes (1800x1169 on a 14" MacBook Pro), so GLFW would
    switch the display to 1920x1200;
  - the borderless window overrides `constrainFrameRect:toScreen:` while it covers the screen. Without
    it, the menu bar pushes the window down when it slides in.
- `GLFW_CURSOR_CAPTURED` is unimplemented on Cocoa, so on macOS capturing keeps the normal cursor, as
  the old patch did.
- `glfwFlashWindow` became `glfwRequestWindowAttention`. The fork's `glfwGetCharForKey` and
  `glfwGetKeyForChar` moved into the driver.
- New: Cmd+Enter and the options menu's fullscreen toggle switch the window live, through
  `OSDriver::setFullscreen` and `applyWindowSettings`. A live switch always uses borderless
  fullscreen at the desktop size. A specific fullscreen resolution still needs a restart.
- The driver tracks `fb_width`/`fb_height` apart from `win_width`/`win_height`. The screen's
  viewport, scissor and screenshots use the framebuffer size, while the UI and the mouse use the window
  size. GLFW reports the backing size even for an unscaled surface, so on macOS the framebuffer size
  comes from the `retinaFramebuffer` flag.

- Gamepads go through `OSDriver::getGamepadState`, which uses GLFW's gamepad API and its built-in
  SDL controller database. GLFW's joystick functions are main thread only and script ticks run on
  the pool, so the driver reads the pads once a frame in `swapBuffers` and hands out a copy. The
  XInput and xboxdrv paths are gone, and so is rumble, which GLFW lacks. The client script never
  polled the pad before (`joystickConnected` was never set), so gamepad input is new in practice.
- Nothing outside `os/` includes GLFW now. `compat/gl.h` only includes GLEW.
- Retina (M4.4) is on, in one step instead of two:
  - the framebuffer has the screen's pixel density (`retinaFramebuffer`);
  - FreeType fonts rasterise at `96 × density` DPI and keep their metrics in points, so layout is
    unchanged. Text widths add up fractional advances, where they used to truncate each one;
  - fonts load after the window, to read the density;
  - the 3D scene keeps rendering at the window's size in points and is scaled up, so the GPU cost is
    unchanged. `bSupersample` renders it at full resolution;
  - the UI renders at full resolution, including through the `dGUIScale` render target;
  - not done: `.fnt` bitmap fonts and image assets stay at 1x, and `GL_LINES` draw one pixel wide.

**Still to do:** the window-mode test matrix below. Windowed, borderless fullscreen and Cmd+Enter
pass. Exclusive fullscreen, more than one monitor, Stage Manager, the console key, the trackpad
and a gamepad are untested.

### M4.1 Build integration
- Add GLFW 3.4 to `source/macos/build_deps.sh`:
  - fetch the release tarball and pin its SHA-256;
  - build with `cmake_build glfw -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF`.
- Point the Makefile at `deps/macos/include` and `deps/macos/lib/libglfw3.a`. Remove:
  - the `libglfw3.a` rule (`source/linux/Makefile:402-403`, `432-435`);
  - `build_glfw` in `source/linux/build.sh`;
  - the `-isystem./source/glfw/include` include (`Makefile:108`) and its comment at `:132`.
- Delete `source/glfw/`, which also removes the stray in-tree `CMakeCache.txt` and `CMakeFiles`.

### M4.2 Port the fork's GLFW patches
Commits `8920b4d` and `1a4040c` patched GLFW 3.0.2's Cocoa code. Check each patch against 3.4. Drop it
if upstream fixed it; otherwise re-implement it in `glfw_driver.cpp`, not by patching GLFW again.

| Patch | What it fixed | 3.4 status (to verify) |
|---|---|---|
| Key table swap | ANSI grave and ISO section keys were swapped, so the console key did nothing on US keyboards | Probably fixed upstream. Test on ANSI and ISO keyboards. |
| Monitor name | Crash when a monitor has no IOKit name | 3.3+ uses `NSScreen.localizedName`. Test with external and virtual displays. |
| Retina size | Rendering used window size | Handled by the hint `GLFW_COCOA_RETINA_FRAMEBUFFER` (see M4.4). |
| Borderless fullscreen | Fullscreen at the desktop mode | Use `glfwSetWindowMonitor` with the current video mode, or a borderless window. Test Spaces and the notch. |
| Cursor visible while captured, warps not suppressing mouse events | Cursor and mouse behaviour while the pointer is locked | Use `GLFW_CURSOR_DISABLED` + `GLFW_RAW_MOUSE_MOTION`. Re-test edge-pan and camera rotation. |
| `sr2PreciseScroll` (commit `0147374`) | Tells the game whether a scroll event came from a trackpad, so zoom can scale trackpad deltas | 3.4 still doesn't expose `hasPreciseScrollingDeltas`. Re-apply in `scrollWheel:`, or read it in `glfw_driver` with an `NSEvent` local monitor so GLFW stays unpatched. |
| `sr2MagnifyCallback` (commit `161f9fb`) | Trackpad pinch to zoom | 3.4 has no gesture API. Re-apply `magnifyWithEvent:`, or use an `NSEvent` local monitor for `NSEventTypeMagnify` in `glfw_driver`. |

### M4.3 API changes in `glfw_driver.cpp`
- Use the framebuffer-size callback for the viewport and render targets, and the window-size callback
  for the UI and input.
- Add `glfwGetWindowContentScale`.
- Move the joystick code behind `OSDriver`:
  - add `OSDriver::getGamepadState(index, state&)`;
  - port `scripts/bind_joystick.cpp` (its `//TODO` at line 90) to it;
  - use GLFW's gamepad API with the SDL controller database. This drops the hand-rolled deadzones.
- Keep `os/key_consts.h` as is. It already equals GLFW's key codes, so scripts and saved keybinds are
  unaffected.
- Put all native-window access in one function, `os::getNativeWindow()` (returning an `NSWindow*` via
  `glfwGetCocoaWindow`), so the Metal layer setup in M6, and any future SDL swap, touches only one
  place.

### M4.4 Retina, in two steps
1. **Parity first:** set `GLFW_COCOA_RETINA_FRAMEBUFFER` to false. It defaults to true in 3.3+, so
   this keeps today's 1x rendering. Ship that.
2. **Retina on:**
   - Set the hint to true.
   - Render the 3D scene at `framebufferSize * scale_3d` (`tick.cpp:551-556`), defaulting to 0.5 on
     2x displays so the GPU cost doesn't double.
   - Render the GUI at native resolution, laid out in points × a UI scale setting.
   - Mouse coordinates arrive in points, so convert them once in the driver.
   - Audit every place that assumes `screenSize` equals the window size: `tick.cpp:547`,
     `redirectWindowResize` (`glfw_driver.cpp:491-495`), and any script reading the screen size.

**Exit:** GLFW 3.4 builds from `build_deps.sh` with `source/glfw/` deleted. The window-mode test
matrix passes:
- windowed
- borderless fullscreen
- exclusive fullscreen
- multi-monitor moves
- Spaces / cmd-tab
- Stage Manager
- cursor lock
- console key on ANSI and ISO keyboards
- gamepad

---

## M5: Make the OpenGL renderer Metal-shaped

The goal is to remove every fixed-function and GL-2.1-only dependency while still on OpenGL, ending on
an OpenGL 4.1 core profile. Every change here is something the Metal backend needs anyway, and doing
it on GL keeps the game playable and comparable.

### M5.1 Close the driver boundary
Done. The leaks and how each was closed:

| Leak | Fix |
|---|---|
| `VertexBufferTCV` was a concrete GL class, used from `tick.cpp`, the scene nodes, `font_ft2.cpp` and `bind_render.cpp`. | The batching (`fetch`/`request`/`draw`, steps, shader caches) is backend-neutral in `render/vertexBuffer.cpp`. Only submission is per backend: `RenderDriver::drawBatch(vertices, indices, steps)`, which GL implements in `gl_vertexBuffer.cpp`. |
| `tick.cpp` cast to `glFrameBuffer` to get the depth texture | `Texture::getDepthTexture()`, null for anything that isn't a render target. |
| `render/lighting.cpp` called `glLight*` directly | `RenderDriver::setLight(index, const LightDesc*)`, null to disable, with `render::MAX_LIGHTS` (8). The old cap used `GL_MAX_LIGHTS`, which is the enum value 0x0D31, not 8. |
| `os/glfw_driver.cpp` called `glGetIntegerv(GL_VIEWPORT)` and `glViewport` | Removed. `renderFrame` sets the framebuffer size and binds the target every frame, which sets the viewport. The window driver now includes GLFW with `GLFW_INCLUDE_NONE`. |
| `Texture::getID()` returns a GL name | Only `gl_driver.cpp` calls it. It stays on the interface as a backend handle. |
| `compat/gl.h` included by `bind_render.cpp` and `line_trail_node.cpp` | Removed; neither used it. |

`initialization.cpp` still includes `render/gl_driver.h` for `createGLDriver()`. That is the backend
factory, and M6 adds the Metal one beside it.

### M5.2 Move fixed-function state onto the CPU
Done on the CPU side; the rest lands with the shim (M5.3):
- **Done:** the transform stack, light and material state live in `GLDriver` as plain data. GL
  only receives whole matrices (`glLoadMatrixd`) and eye-space light positions, so `gluPerspective`,
  `gluLookAt`, `glOrtho`, `glPushMatrix`/`glMultMatrixd` are gone. `Matrix` gained `ortho`, `lookAt`
  and `scale`. The batch limit is 16384 vertices.
- **Moved to M5.3:** the `SRBuiltins` block, the alpha-test uniform and generic attributes with a VAO
  for `VertexBufferTCV`. All three only take effect once shaders are rewritten, because GLSL 1.20
  reads `gl_*` state and fixed-function vertex pointers. Meshes already use generic attributes 0-5
  with VAOs (attribute 0 aliases `gl_Vertex`).
- **Not needed:** quads are already indexed triangles in `VertexBufferTCV`, and meshes have no quads.
  16-bit indices stay; a batch is well under 65536 vertices.

The original plan follows.

- **Matrices:**
  - The driver keeps projection, view and a model stack as `double` matrices.
  - `setTransformation`/`resetTransformation`/`setBBTransform` push and pop on that stack.
  - Remove `gluPerspective`/`gluLookAt` (`gl_driver.cpp:~1185`) and `glMultMatrixd` in
    `scene/mesh_node.cpp:33-39`.
  - Per draw, the driver derives `modelView`, `modelViewProjection` and `normalMatrix` as floats,
    camera-relative. Positions stay in doubles on the CPU until the subtraction, as GL already does
    implicitly.
- **Built-in uniform block:** add one shared `SRBuiltins` uniform block, used by every shader:

  ```glsl
  struct SRLight { vec4 position; vec4 diffuse; vec4 specular; vec4 ambient;
                   float constantAttenuation, linearAttenuation, quadraticAttenuation, _pad; };
  struct SRMaterial { vec4 diffuse; vec4 specular; vec4 ambient; vec4 emission; float shininess; };
  struct SRLightModel { vec4 ambient; };
  layout(std140) uniform SRBuiltins {
      mat4 sr_ModelView, sr_Projection, sr_ModelViewProjection;
      mat3 sr_NormalMatrix;          // std140 pads to 3 x vec4
      SRLight sr_Lights[8];
      SRMaterial sr_Material;        // from RenderState.diffuse/specular/shininess
      SRLightModel sr_LightModel;
      float sr_AlphaRef; int sr_AlphaTest; int sr_LightCount; float _pad;
  };
  ```

  Fill the exact fields from what the 140 shaders actually read: `gl_LightSource` (43 files),
  `gl_FrontMaterial` (15), `gl_LightModel` (14).
- **Alpha test:** `glAlphaFunc` becomes `sr_AlphaTest`/`sr_AlphaRef`, applied by the shim in the
  fragment shader epilogue (`render_state.h` notes alpha test is always > 0).
- **Vertex arrays:** client-side vertex arrays (`gl_vertexBuffer.cpp:220-226`) become VBOs + VAOs.
  Meshes get one VAO each (`gl_mesh.cpp:85-127`).
- **Quads:** `PT_Quads` becomes indexed triangles, using a static index pattern buffer
  (0,1,2, 0,2,3, ...) in `VertexBufferTCV` and mesh upload.
- **Batch size:** raise `maxVerts` (`gl_vertexBuffer.cpp:7`) from 2048 to 16384, and the index type
  from `unsigned short` to `uint32_t` where needed.
- **Wireframe:** `glPolygonMode` stays available in GL core.

### M5.3 The shader shim
Done. `render/shader_shim.cpp` has two targets:
- **GLSL 1.20 (`ST_GLSL120`), live in the game.** Every shader compiles through it. Built-in state
  comes from `sr_*` uniforms that `GLShaderProgram::uploadBuiltins` fills from `BuiltinState`
  (`render/shader_builtins.h`) before each draw, when its serials have changed. If a shimmed shader
  fails to compile, the original is used and the log says so. Fixed-function state is still uploaded
  too, for draws without a shader.
- **GLSL 4.10 core (`ST_GLSL410`), for M5.5 and the translator.** Use `shimProgram`, which shims
  both stages together.

What the shim does beyond the original list:
- Declares only the `sr_*` built-ins a shader uses.
- Wraps `texture2D`/`textureCube` calls (`sr_texture2D`, ...), because many shaders name a sampler
  `texture`, which hides the core function.
- Renames names that are core GLSL keywords (`sample` in 8 shaders) or Metal built-ins (`atan2` in
  the ringworld shader) to `sr_kw_*`.
- Turns fragment varyings the vertex shader never declares into plain globals (9 programs). GL left
  them undefined; Metal can't link them.
- Vertex shaders always output `sr_FrontColor`, so any fragment shader reading `gl_Color` links.
- Attribute locations are fixed (`ShaderAttribute`), including `in_position`, which 9 shaders used
  without a binding and only worked because the linker put it at 0. Batches feed generic attributes
  as well as the fixed-function arrays.
- `glUseProgram` is skipped when the program is already bound.

The original plan follows.

Lives in `render/shader_shim.cpp` (backend-neutral) and is called from `preProcessShader` in
`gl_shader.cpp`. It takes GLSL 1.20 source, including mod shaders, and produces modern GLSL:

1. **Tokenise.** Use a small GLSL tokenizer that respects comments and preprocessor lines, not raw
   regex. Strip `#version`.
2. **Prepend a stage-specific prelude**, which carries most of the mapping as macros:
   ```glsl
   #define attribute in                      // VS only
   #define varying   out / in                // per stage
   #define texture2D   texture
   #define textureCube texture
   #define gl_ModelViewMatrix           sr_ModelView
   #define gl_ProjectionMatrix          sr_Projection
   #define gl_ModelViewProjectionMatrix sr_ModelViewProjection
   #define gl_NormalMatrix              sr_NormalMatrix
   #define gl_LightSource               sr_Lights
   #define gl_FrontMaterial             sr_Material
   #define gl_LightModel                sr_LightModel   // struct with .ambient
   #define ftransform() (sr_ModelViewProjection * sr_Vertex)
   ```
3. **Fixed attributes:**
   - In the vertex shader, `gl_Vertex`, `gl_Normal`, `gl_MultiTexCoord0` and `gl_Color` become
     declared inputs (`sr_Vertex`, `sr_Normal`, `sr_TexCoord0`, `sr_Color`) at fixed locations.
     These must match `VertexTCV` and the mesh layout.
   - The engine's own named attributes (`in_position`, `in_normal`, `in_uv`, see
     `smooth_light_vs.txt`) get the same fixed locations.
4. **Varyings and outputs:**
   - `gl_FrontColor`/`gl_BackColor` become `out vec4 sr_FrontColor`.
   - In the fragment shader, `gl_Color` becomes `in vec4 sr_FrontColor`.
   - `gl_FragColor` becomes `out vec4 sr_FragColor`.
   - Inject an alpha-test epilogue by renaming the user's `main` to `sr_user_main` and adding a
     wrapper `main`.
5. **Unsupported built-ins.** Fail with a clear log line naming the mod file and built-in.
   `gl_TexCoord[]` (1 file) and `gl_FragData` (0) need explicit cases.
6. **Output:**
   - `#version 410 core` for GL (M5);
   - `#version 450` with Vulkan relaxed rules for glslang (M5.4/M6).

Loose `uniform` declarations stay as they are:
- GL accepts them.
- For glslang, enable relaxed Vulkan rules (`TShader::setEnvInputVulkanRulesRelaxed`, with a global
  uniform block name and binding). These collect loose uniforms into one block, and SPIRV-Cross
  reflection reports their offsets. *Verify this API against the pinned glslang version.*

Remove `data/shaders/source/fs_test_ps.txt` (`#version 130`) if it is unused, or shim it.

### M5.4 Translation toolchain and validator
Done:
- `build_deps.sh` builds glslang and SPIRV-Cross at `vulkan-sdk-1.4.357.0`, with their command-line
  tools in `deps/macos/bin`.
- `render/shader_translate.cpp`: `translateProgram` runs a shimmed program through glslang
  (Vulkan relaxed rules, global block `SRUniforms`) and SPIRV-Cross (MSL 3.0, `fixup_clipspace`),
  with entry points `sr_vertex`/`sr_fragment`. Fragment inputs take the location of the vertex
  output with the same name. Reflection names uniforms as GL does (`sr_LightSource[1].diffuse`).
  Both stages of a program get the same uniform block layout, so M6 can bind one buffer to both.
- `make -f source/linux/Makefile shadertool` builds and runs `bin/osx64/shadertool`, which takes
  every program through to `xcrun metal`. All 93 base-game programs pass. `--mod <folder>` adds a
  mod's shaders. The game binary doesn't link glslang yet.

The original plan follows.

- Add glslang and SPIRV-Cross to `build_deps.sh`, as static libraries with pinned release tags and
  SHA-256. Build only the needed targets: glslang with SPIR-V output; SPIRV-Cross core, MSL and
  reflect.
- New `render/shader_translate.cpp`:
  - API: `translate(shimmedGLSL, stage) -> { msl, reflection }`.
  - Reflection covers uniform names, offsets and sizes, texture binding indices, and vertex input
    locations.
  - SPIRV-Cross MSL options: `msl_version` 3.0 (macOS 13), `platform = macOS`, argument buffers off
    to start.
- New `tools/shadertool` CLI (built by the Makefile) that runs every `Shader:` entry in
  `data/shaders/*.txt`, and optionally a mods folder, through shim, glslang, SPIRV-Cross and
  `xcrun metal -c`. It prints failures. Running it in M5 catches translation problems before the
  Metal backend exists, and it is the future CI check.

### M5.5 Switch to an OpenGL 4.1 core profile
The game runs on a 4.1 core, forward-compatible context. It took these steps, each run in-game:
1. **FixedFunction shader** (`data/shaders/source/fixed_function_*.txt`, owned by `GLDriver`) for
   every material without a shader: about 200 data materials, all fonts and the console. It follows
   the GL 2.1 lighting equation and `GL_MODULATE`, with alpha-only textures modulating only alpha.
   Textures that aren't loaded yet draw untextured, as fixed-function GL did.
2. **Alpha test** in the fragment epilogue (`sr_AlphaTest`, `sr_AlphaRef`) for both shim targets.
3. **No fixed-function state:** matrices, lights and materials go only to shaders; vertices only
   through generic attributes.
4. **Texture state:** no per-unit enables, no EXT direct state access, `TW_Clamp` is
   `GL_CLAMP_TO_BORDER`, no mipmap hint.
5. **The switch:** `<OpenGL/gl3.h>` with no GLEW (removed from the build), shaders compiled from
   `shimProgram` output, a vertex array for every mesh and one shared by batches, and grey/alpha
   images as `GL_R8` swizzled to the values the old formats gave shaders.

Also done:
- **120 Hz with VSync on.** OpenGL on macOS presents at no more than 60 Hz, even on a 120 Hz
  ProMotion display. On displays above 60 Hz, "VSync on" now uses swap interval 0 and the frame
  limiter paces to the display's refresh (checked again when the window moves). The limiter paces
  from its previous deadline, so waking late no longer lowers the frame rate.
- **Scissor:** `pushScreenClip`/`popScreenClip` flush the batch first, so batched geometry is never
  clipped by a later scissor.
- **Unit 0:** FixedFunction gets a 1x1 white texture when it samples nothing, which silences Apple's
  "unit 0 ... unloadable" warning. M6 can use the same texture.
- **`glGetError`:** the per-frame checks were already debug-only. The rest run while loading
  textures, meshes and framebuffers, so they stay in release, where they report real errors.

The original plan follows.

- `glfw_driver.cpp:163-164`:
  - `GLFW_CONTEXT_VERSION_MAJOR/MINOR` = 4/1;
  - `GLFW_OPENGL_PROFILE` = `GLFW_OPENGL_CORE_PROFILE`;
  - `GLFW_OPENGL_FORWARD_COMPAT` = true.
- Drop GLEW and include `<OpenGL/gl3.h>`. macOS needs no loader for core GL. This also removes GLEW
  from `build_deps.sh`.
- `glGetError`/`reportErrors`: keep them in debug builds, compile them out in release.
- Fixes from the audit that fall out naturally here:
  - `glUseProgram` only when the program changes (`gl_shader.cpp:363-366`);
  - flush the batch before `glScissor` changes (`gl_driver.cpp:1344-1360`).

### M5.6 Visual parity harness
- Add a `--capture <scenario>` launch option that:
  1. loads a fixed save;
  2. sets a fixed camera list, time and effects seed;
  3. renders N frames;
  4. writes PNGs.
- Scenarios:
  - main menu
  - galaxy overview
  - system close-up with planets/atmospheres
  - a fleet battle with beams, particles and shields
  - the design editor
  - the GUI-heavy planet screen
- Compare against the M0 GL 2.1 captures with a perceptual diff tolerance. Commit the reference images
  under `docs/parity/` (or keep them out of the repo if they're large).

**Exit:** the game runs on a GL 4.1 core context with every built-in shader through the shim.
`shadertool` translates all built-in shaders to MSL. Parity captures match within tolerance. No
regression on the benchmark.

---

## M6: Metal backend

### M6.1 Layout and build
```
source/game/render/mtl/
  mtl_driver.mm        RenderDriver implementation, frame lifecycle, passes
  mtl_state.mm         pipeline / depth-stencil / sampler caches
  mtl_buffer.mm        per-frame ring buffer, batch (VertexBufferTCV backend)
  mtl_texture.mm       Texture, cubemaps, render targets, readback
  mtl_mesh.mm          RenderMesh (static vertex/index buffers)
  mtl_shader.mm        ShaderProgram: translated MSL -> MTLLibrary, reflection binding
  mtl_layer.mm         CAMetalLayer attach to NSWindow (via os::getNativeWindow)
```
- Makefile:
  - add a `.mm` rule with `-fobjc-arc`;
  - link `Metal`, `QuartzCore`, `Foundation`, `AppKit`;
  - remove `OpenGL` in M7.
- Choose the backend with the `render_backend` setting (`metal` | `gl`), defaulting to `gl` until
  parity, then to `metal`.

### M6.2 Window and frame pacing
- Create the window with `GLFW_CLIENT_API = GLFW_NO_API` when the backend is Metal.
- Attach a `CAMetalLayer` to the content view:
  - `pixelFormat = BGRA8Unorm`. Use `_sRGB` only if the GL path was gamma-correct; it wasn't, so
    match the GL output.
  - `framebufferOnly = YES`, except when a screenshot is pending.
  - `maximumDrawableCount = 3`.
  - `displaySyncEnabled` maps to the vsync setting.
  - `contentsScale` follows the window's backing scale.
- **In-flight frames:** a `dispatch_semaphore(3)`. Wait at frame start, signal in the command buffer's
  completion handler.
- **Drawable:** acquire it with `nextDrawable` as late as possible, just before the final on-screen
  pass, to minimise the time we hold it.
- **Frame cap:** when set and vsync is off, use `sleepUntil` from M2. It is no longer a spin.

### M6.3 Mapping the driver model
The driver API is immediate-mode: `switchToRenderState`, then draw. Metal is retained-state and works
in passes. The backend bridges them with lazy state.

| Concept | Metal implementation |
|---|---|
| Render pass | One `MTLRenderCommandEncoder` per target. `setRenderTarget` ends the current encoder and opens a new one. `clearScreen` at pass start uses `loadAction = Clear`; otherwise it draws a fullscreen clear quad. |
| `RenderState` | **Pipeline key:** shader, `baseMat` (blend), vertex layout id, color format, depth format, sample count. **Depth-stencil key:** `depthTest`, `depthWrite`. **Encoder state:** `culling`, `drawMode`. **Sampler key:** wrap, filter, `mipmap`. Each is hashed into its own cache. |
| Blend modes | `MAT_Solid`, `MAT_Add`, `MAT_Alpha`, `MAT_Font` and `MAT_Overlay` become fixed `MTLRenderPipelineColorAttachmentDescriptor` presets, copied from the GL `glBlendFunc`/`glBlendFuncSeparate` calls. |
| `SRBuiltins` uniform block | Fill a CPU struct per draw and call `setVertexBytes`/`setFragmentBytes` (it fits in 4 KB), or copy it into the ring buffer and bind it by offset. |
| `shaders.txt` variables (`node_color`, `game_time_cycle`, `global`, ...) | Pack them into the shader's reflected uniform layout from SPIRV-Cross, using the existing `Shader::bind(float* dynamicBuffer)` data. Constant variables are cached per program. |
| `textures[8]` | `setFragmentTexture` at the reflected indices, plus a cached `MTLSamplerState`. |
| `VertexBufferTCV` batches | Write into a per-frame shared-storage ring buffer (about 8 MB, growable) with 3 frames rotating. Draws use offsets into it, and quads use the static index buffer. No orphaning or uploads, thanks to unified memory. |
| `RenderMesh` | Static `MTLBuffer`s, private storage, uploaded once with a blit. |
| Scissor (`pushScreenClip`) | `setScissorRect`, flushing the batch first. |
| `getScreen` (screenshots) | Render to a non-framebufferOnly target, blit to a shared buffer, wait on that command buffer only. |
| Lines (`PT_Lines`, `PT_LineStrip`) | `MTLPrimitiveTypeLine`/`LineStrip`. Width is always 1. GL has no `glLineWidth` uses, so nothing is lost. |

### M6.4 Pipeline creation without hitches
- When `shaders.txt` loads:
  1. translate every shader (using the disk cache);
  2. call `newLibraryWithSource` asynchronously;
  3. pre-create pipelines for the (shader × blend × layout) combinations the materials actually
     reference. The material files define them, so the set is known.
- A pipeline missing at draw time is created synchronously and logged. The log tells us which keys to
  pre-create.
- Later: an `MTLBinaryArchive` in the profile cache, to skip backend compilation on warm starts.

### M6.5 Translation cache
- **Key:** hash of (shim version, shimmed source, stage, SPIRV-Cross options, pinned library
  versions).
- **Value:** MSL source + a reflection blob, stored under the profile directory (next to the existing
  script `as_cache` location).
- A version bump of the shim or a library invalidates everything.

### M6.6 Textures
- `MTLDevice` is thread-safe, so loader workers create the `MTLTexture` and upload with
  `replaceRegion`, or use a blit encoder on a separate queue for mipmaps.
- Upload no longer happens on the main thread, so the 4 MB-per-frame upload budget and the
  `lod == 0` full-upload hitch (`load_material.cpp:247-250`) go away.
- Signal completion to the main thread with an atomic "ready" flag on the texture.
- Render targets are private textures with the `RenderTarget | ShaderRead` usage. The depth target is
  a separate texture, which removes the GL framebuffer-isn't-a-texture hack.

### M6.7 Bring-up order (each step demoable)
1. A clear colour and the 2D path: `drawQuad`/`drawRectangle`/fonts/skin. The **main menu renders.**
2. Textures and materials, the GUI with clipping. **All menus and the lobby work.**
3. Camera, meshes, lighting, depth. **Galaxy and system views.**
4. Billboards, particles, beams, line trails. **Combat.**
5. Render targets, the fullscreen post pass (`tick.cpp:680-700`), the skybox cubemap.
6. Screenshots, resize/fullscreen/Retina, and the parity harness passing for all scenarios.

### M6.8 Debugging aids
- Label every encoder, pipeline and buffer, so Xcode's GPU frame capture is readable.
- `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` in debug runs.
- Add a `render_backend` switch to the console for A/B testing.

**Exit:** the parity harness passes on Metal. The benchmark frame time is equal to or better than
GL 4.1. No missing features. Metal becomes the default.

---

## M7: Metal-only wins, then remove OpenGL

Do these in order of measured benefit:

1. **Draw ordering:** sort opaque draws by pipeline, then texture set, then mesh. Today
   `node.cpp:595-608` sorts opaque draws by distance. Keep back-to-front ordering only for
   transparent draws.
2. **Instancing:** ship and planet meshes that share a mesh and material become one
   `drawIndexedPrimitives:instanceCount:` with per-instance transforms in the ring buffer. This is
   where large late-game fleets benefit most.
3. **Particles:** frustum-cull particle systems by a bounding sphere (radius = life × speed). Today
   `particle_system.cpp:526` culls nothing. Pool particles in structure-of-arrays storage instead of
   `new Particle` per particle. Use precomputed billboard vectors when rotation is 0.
4. **Texture formats:** cache textures in ASTC or BC, pre-baked with mip chains, in the profile
   cache. Startup decodes about 490 MB of PNG today.
5. **Hand-written MSL (shader option A)** for the hottest translated shaders (ships, planets,
   atmospheres, particles). Keep translation as the fallback for mods.
6. **Later options:** argument buffers, and parallel encoding with `MTLParallelRenderCommandEncoder`.
7. **Remove the OpenGL backend:** delete `gl_*.cpp` and the GL context code, and drop the `OpenGL`
   framework.

---

## iOS readiness

Not planned yet. These notes keep the current work from blocking it.

- **Order:** iOS has no OpenGL, so it needs M6. GLFW has no iOS support, so it also needs a new
  platform layer: SDL3 (window, touch, gamepad and audio on both platforms) or a small UIKit layer.
  Decide when iOS work starts. SDL3 is the smaller change if it replaces GLFW on macOS too.
- **Platform boundary:** keep all platform code behind `OSDriver` and in `os/`. Native window code
  lives in `os/cocoa_window.mm`. Nothing outside `os/` includes GLFW.
- **Input:** trackpad gestures (pinch, rotate, two-finger pan) become their own driver events, which
  scripts handle in one place. Touch will feed the same events.
- **UI work, the largest part:** the interface assumes a mouse and keyboard:
  - hover tooltips: touch has no hover, so tap to preview;
  - right-click context menus: long press;
  - drag box-select: a selection mode or a two-finger drag;
  - keyboard shortcuts: on-screen controls for the common ones;
  - small hit targets: a touch UI scale, which `dGUIScale` can carry.
- **Other:** sandboxed file paths for profiles and saves, app lifecycle (suspend saves and stops the
  simulation), no Workshop or curl downloads, and memory limits for textures (M7.4's compressed
  formats help).
- **Licensing:** the earlier question about shipping on the App Store applies.

---

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| A hidden cross-thread mutex release surfaces as an `os_unfair_lock` abort | Medium | The M2.2 audit run. Give that call site a semaphore or `Signal`. |
| Code relies on spin timing, for example `try_lock` failing quickly or LIFO action order | Medium | Keep `try_lock` semantics. Change `popAction` to FIFO in its own commit and benchmark it. |
| Async script calls block inside pool jobs and deadlock | Medium (M3.5 phase 2) | Keep empire threads dedicated until async returns stop blocking. |
| The shim can't express a mod shader's use of compat built-ins | Medium | Log a clear error naming the file and built-in. `shadertool` run over popular Workshop mods. A per-shader GL fallback while the GL backend still exists. |
| glslang's relaxed Vulkan rules don't behave as expected for loose uniforms | Low-Medium | Validate in M5.4 before relying on it. The fallback is for the shim to gather loose uniforms into a block itself. |
| Precision differences in camera-relative transforms | Low | Keep doubles on the CPU until the subtraction, as today. Parity captures at extreme zoom. |
| Retina doubles GPU cost | Medium | `scale_3d` defaults to 0.5 on 2x displays, and the UI renders at native resolution. |
| GLFW 3.4 behaviour differences in fullscreen/Spaces | Medium | Ship M4.4 step 1 (parity) separately and run the window-mode test matrix. |
| C++20 switch exposes ambiguous `operator==` or other breakage | Low | Done first (M0.1), on its own. |

## Open questions

1. Which Workshop mods ship `.txt` shaders, and which built-ins do they use? Run `shadertool` over a
   sample before M6.
2. Exact `SRBuiltins` fields: resolved by a scan of `data/shaders/source` (141 files). Shaders read:
   - matrices: `gl_ModelViewMatrix`, `gl_ProjectionMatrix`, `gl_ModelViewProjectionMatrix`,
     `gl_NormalMatrix`, and `gl_ProjectionMatrixInverse` (once, `fs_post_process_vs.txt`);
   - `gl_LightSource[]`: `position`, `diffuse`, `specular`, and all three attenuations (no
     `ambient`); index 0, 1 or a loop variable;
   - `gl_FrontMaterial`: `ambient`, `diffuse`, `specular`, `shininess` (no `emission`). Ambient is
     always white: GL sets it once from the default diffuse;
   - `gl_LightModel.ambient` (0.175 grey);
   - inputs `gl_Vertex`, `gl_Normal`, `gl_Color`, `gl_MultiTexCoord0`; outputs `gl_FrontColor`,
     `gl_BackColor`, `gl_FragColor`, and `gl_TexCoord[0]` in one shader pair.

   `GLDriver` already keeps all of these on the CPU.
3. Script render bindings: resolved in M5.1. They go through `RenderDriver` and `RenderState`, and
   the bound enums are engine values, not GL ones. GL semantics the Metal backend has to reproduce:
   - light positions are transformed by the model transform current when `light.enable()` runs;
   - `PointLight.att_constant`/`att_linear` are bound but GL never applied them (only quadratic);
   - `FC_Both` culls everything (Metal has no such mode: skip the draw);
   - `TW_Clamp` is `GL_CLAMP` (Metal: clamp to border colour);
   - `NormalizeNormals` and `DM_Line` wireframe.

   No base-game material uses `Clamp`, `Both` or `NormalizeNormals`, but mods may.
4. Retina UI scale: resolved. The UI is laid out in points and drawn at the backing scale factor, and
   `dGUIScale` stays a user setting on top.
5. Does the effects RNG need a fixed seed option for the parity harness, or is there already one?

## Key files

- **Threading:** `source/os/include/threads.h`, `source/os/source/threads.cpp`,
  `source/os/source/threads_gcc.cpp`, `source/game/processing.cpp`, `source/game/obj/lock.cpp`,
  `source/game/main/tick.cpp`, `source/game/memory/AllocOnlyPool.h`.
- **Rendering:** `source/game/render/driver.h`, `render/gl_*.cpp`, `render/lighting.cpp`,
  `render/vertexBuffer.h`, `render/render_state.h`, `render/shader.h`, `scene/mesh_node.cpp`,
  `scene/node.cpp`, `scene/particle_system.cpp`, `data/shaders/*.txt`, `data/shaders/source/`.
- **Platform:** `source/game/os/glfw_driver.cpp`, `source/game/os/key_consts.h`,
  `source/game/scripts/bind_joystick.cpp`.
- **Build:** `source/linux/Makefile`, `source/linux/build.sh`, `source/macos/build_deps.sh`,
  `source/macos/make_app.sh`.
