# Stop-hang cancellation design

Status: design approved (sections), pending written-spec review
Date: 2026-05-31
Repos touched: `moqxr` (sibling `../moqxr`) and `moq2ts` (this repo)

## Problem

Stopping a live camera publish freezes the UI; the app must be force-closed. The
publisher stdout simply stops mid-stream with no graceful shutdown output,
consistent with the process being killed while hung.

Root cause (verified against the real code, not assumed):

The pipeline worker thread runs `LivePipeline::runLoop`
(`src/media/LivePipeline.cpp`) which calls
`MoqxrPublisher::publishLiveObjects` -> `Publisher::publish_live_objects`
(`../moqxr/src/publisher_api.cpp:242`) -> `MoqtSession::publish_live_objects`
(`../moqxr/src/transport/moqt_session.cpp:3721`). That method runs a blocking
loop at `moqt_session.cpp:4059`:

```cpp
while (!source_eof) {
    ...                                                   // process_control_messages + read_stream (4072)
    std::optional<LiveObject> next = source.next_object();// 4098 - only stop signal, via m_running
    if (!next.has_value()) { source_eof = true; break; }  // 4099
    ...
    status = sender_by_track[next->track_name].serve(...);// 4164 - BLOCKING network send
}
```

`MoqtSession` has **no cancellation flag**. The loop only winds down when
`source.next_object()` returns `nullopt` (our `nextObject` lambda does this when
`m_running` is false). But:

1. The camera delivers ~5 fps, so between objects the loop sits in
   `serve()` / `read_stream()` / pacing and does not call `next_object()` again
   for up to ~200 ms+.
2. When the relay stalls (the observed 60 s inactivity / queue backpressure),
   `serve()`'s network write can block indefinitely - and it blocks *before* the
   loop reaches `next_object()` again, so `m_running = false` is never observed.

`MoqtSession::close()` (`moqt_session.cpp:4225`) resets streams and calls
`transport_.close()`, but the publish loop never checks transport/connection
state, so `close()` from another thread does not break the loop today.

The freeze the user sees: `main.cpp:48` runs `pipeline.stop()` ->
`waitForStopped()` -> `m_workerThread.join()` (`LivePipeline.cpp:107`), an
unconditional, timeout-less join on a worker that never returns. `main.cpp:54-60`
(`aboutToQuit`) also calls `waitForStopped()` on the GUI thread, so even
force-quitting wedges the UI.

The `21b791a` "stop wakeup" commit added transport `picoquic_packet_loop_wake_up`
handling and catalog ordering, but did **not** add a session-level stop flag, so
this gap remains.

## Decision

Fix both layers; each is independently correct.

- **moqxr:** make `publish_live_objects` cancellable via an atomic stop flag in
  `MoqtSession` that `close()` sets and the loop observes, with a bounded
  graceful flush before returning.
- **moq2ts:** guarantee the GUI thread never blocks at Stop via a bounded join
  with a detach fallback, and bound the `aboutToQuit` teardown.

Chosen behavior (from brainstorming): graceful flush of the in-flight
object/group, bounded (~2 s) with a fall-back to hard cancel; UI frees within
~1-2 s. Cancellation mechanism: an atomic flag in `MoqtSession` (no public API
signature change). The capture-side wait is handled by the existing
`LibavCaptureSource::readObject` poll of the `running` atomic - no new
cross-layer plumbing.

## Design

### Layer A: moqxr `MoqtSession` cancellation

**A1. New member** (`../moqxr/include/openmoq/publisher/transport/moqt_session.h`,
private block near `transport_`); add `<atomic>` to includes:

```cpp
std::atomic<bool> stop_requested_{false};
```

Atomic because `close()` and the publish loop run on different threads. It is a
one-way latch with no dependent state, so acquire/release ordering suffices.

**A2. `close()` sets it first** (`moqt_session.cpp:4225`). The first statement of
`close()` becomes:

```cpp
stop_requested_.store(true, std::memory_order_release);
```

before the existing stream-reset / `transport_.close()` work. Ordering matters:
the flag becomes visible before the transport starts failing reads, so the loop
sees a clean stop rather than an ambiguous transport error.

**A3. The publish loop observes it** (`moqt_session.cpp:4059`, `while
(!source_eof)`):

- Top of loop, before `process_control_messages()`:
  `if (stop_requested_.load(std::memory_order_acquire)) break;`
- After the blocking `read_stream` (4072) and after each `serve()` (catalog send
  at 4120, media send at 4164): if `stop_requested_` is set, `break` to the
  post-loop flush instead of pulling the next object.

`break` (not `return`) lets the existing post-loop block run (`finish_group` at
4181, `PUBLISH_DONE` at 4189) - that block *is* the graceful flush.

**A4. Bound the flush.** The post-loop `finish_group` / `PUBLISH_DONE` writes can
themselves block on a wedged transport. When stop is first observed, capture
`auto flush_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);`
and have those writes use a short per-call timeout and skip remaining flush work
once the deadline passes. Meanwhile the app has already called `close()` on
another thread, tearing the transport down. Net: clean flush when healthy,
prompt return when not.

**A5. Return value.** A stop-initiated exit returns `TransportStatus::success()`
(a user stop is not a failure). A `serve()`/`read_stream` error with
`stop_requested_` **unset** still returns `failure(...)` exactly as today, so
genuine publish errors are unaffected. The moq2ts side guards its error emit
behind `if (m_running)`, so a success return is handled correctly either way.

The transport `picoquic_packet_loop_wake_up` path added in `21b791a` is reused so
a blocked `read_stream` actually wakes when `close()` runs; no new transport
work is required for that.

### Layer B: moq2ts bounded shutdown

**B1. Bounded join** (`src/media/LivePipeline.cpp`, `waitForStopped`). Replace
the unconditional `m_workerThread.join()` with a timed wait of ~3 s. Since
`std::thread` has no timed join, wait on a completion signal the worker raises
(B2) via a `std::condition_variable` + a "worker finished" bool (or a
`std::future` the worker fulfils). On success -> `join()` (instant). On timeout
-> log a warning and `detach()`. Must be idempotent: after a detach,
`m_workerThread.joinable()` is false, so a second call is a no-op.

**B2. `runLoop` signals completion.** The worker sets the completion flag/promise
as its final statement (after `m_running.store(false)` and `emit
status("Pipeline exiting.")`), so B1 waits on a real condition rather than
polling.

**B3. `main.cpp` `aboutToQuit`** (lines 54-60): call the same bounded
`waitForStopped()` and bound the `shutdownFuture.wait()` similarly, so GUI-thread
teardown cannot hang.

**Invariant:** the GUI thread does exactly two synchronous things at Stop - flip
an atomic (`requestStop`) and spawn the async stop task - then returns to the
event loop. Every potentially blocking action (disconnect, join) runs on the
async task and is itself bounded.

### Layer C: lifetime safety for a detached worker

A detached worker can outlive `stop()` and still holds a raw `m_publisher`
pointer and emits queued Qt signals, so on app exit it could touch freed objects.
Mitigations:

- **Ordering:** `publisher.stop()` (which sets moqxr's flag via `close()`) runs
  before the join wait, so by the time B1 would detach, the worker is already
  unwinding. Detach is the rare last resort, not the norm.
- **Signal safety:** `window` / `publisher` / `pipeline` are `main()` stack
  objects that outlive `app.exec()`. `aboutToQuit` runs while they are alive and
  its bounded wait gives the worker its exit window. Queued signals delivered
  after the event loop stops are dropped by Qt (safe).
- A detached worker that overruns the bound is logged as a defect signal (it
  should not happen once Layer A is in place), not silently tolerated.

## Data flow (end-to-end Stop)

Happy path (network healthy):

1. GUI: `handleStop()` -> `emit stopRequested()`.
2. GUI (`main.cpp:37` lambda): `pipeline.requestStop()` sets `m_running=false`
   (instant); launch async stop task; GUI returns to event loop.
3. async: `publisher.stop()` -> `disconnect(0)` -> `MoqtSession::close(0)` sets
   `stop_requested_=true` (A2), `transport_.close()` + packet-loop wake_up.
4. worker: next loop iteration sees `stop_requested_` (A3) -> `break` -> bounded
   post-loop flush (A4) -> returns success. (Belt-and-suspenders: our
   `nextObject` also returns `nullopt` because `readObject` polls `m_running`.)
5. worker: `runLoop` raises completion signal (B2), emits "Pipeline exiting."
6. async: `pipeline.stop()` -> `waitForStopped()` sees worker done -> `join()`
   instant (B1).
7. async: `QMetaObject::invokeMethod(window, "Stopped by user", QueuedConnection)`.
8. GUI: status updates; never blocked.

Wedged path (transport stuck mid-`serve`): identical through step 3. At step 4
the worker is blocked in `serve()`/`read_stream`; `close()`'s `transport_.close()`
+ wake_up makes that call return an error -> loop breaks. If the flush writes are
also stuck, the 2 s deadline (A4) fires and the loop returns anyway. If the
worker is parked in `nextObject()` waiting for a frame, `m_running=false` makes
`readObject`'s poll return `nullopt` within ~one frame interval. All cases exit
well under the 3 s bound -> B1 joins normally.

Pathological path (worker truly will not exit): step 6's `waitForStopped()` times
out at 3 s -> logs warning -> `detach()` -> async task completes -> UI frees.
Expected unreachable once Layer A lands.

## Error handling

moqxr:
- Stop during connect/setup (before the loop): caught at the loop top on the
  first iteration; connect/setup failures keep their existing `failure(...)`.
- Flush writes fail after stop (transport already closing): swallowed - expected,
  not a real error; function still returns `success()` (A5).
- Stop vs genuine failure: the loop converts to `success` only when
  `stop_requested_` is set; a `serve()` error with the flag unset still returns
  `failure(...)`.

moq2ts:
- Bounded-join timeout -> detach: logged at warning level with a greppable marker
  `[moqxr][stop] worker join timed out after 3000ms; detaching`. A diagnostic
  signal that Layer A regressed, not a normal outcome.
- Double-stop / re-entrancy: `stop()` is already guarded (`m_running` check;
  `MoqxrPublisher::stop` early-returns when `!m_connected`); `waitForStopped()` is
  idempotent; the `main.cpp` Stop lambda coalesces overlapping stops via the
  `shutdownFuture` state check.
- Error-emit suppression: the existing `if (m_running.load()) emit error(...)`
  guard stays, so a stop-driven return does not surface a spurious "Failed to
  publish" dialog.

Lifetime/teardown: see Layer C. The design does not rely on detaching during
normal operation; detach is the audited last resort.

## Testing

moqxr (has a real C++ test harness; transport is behind the `PublisherTransport`
/ `TransportFactory` seam, so cancellation is unit-testable without a network;
follow the framework used by `tests/moqt_session_test.cpp`):

- **Test 1 - stop breaks the loop deterministically.** Mock `PublisherTransport`
  + a `LiveObjectSource` whose `next_object` returns objects forever. Run
  `publish_live_objects` on a worker thread; from the test thread call
  `session.close(0)`; assert it returns `success()` well under the 2 s flush
  deadline. This is the regression test for the exact hang: it hangs forever
  against current code, passes against the fix.
- **Test 2 - graceful flush happened.** Healthy mock transport; after stop assert
  the mock recorded a `finish_group` / `PUBLISH_DONE` write before return (A4
  ran).
- **Test 3 - flush deadline fallback.** Mock transport whose `write_stream` /
  `read_stream` block until released; trigger stop; assert `publish_live_objects`
  returns within ~deadline + slack even though flush writes never complete
  (hard-cancel fallback).
- **Test 4 - no false positive.** A `serve()` error with `stop_requested_` unset
  still yields `failure(...)` (A5 boundary).

moq2ts (no C++ unit harness; tests are bash structural guards + the Docker build
as the compile/link check). Add `tests/stop-shutdown-guards.sh` asserting:

- `waitForStopped` contains a bounded wait + `detach()` fallback (not a bare
  `join()`).
- the join-timeout warning marker string is present.
- `runLoop` raises the worker-completion signal (B2).
- `main.cpp` `aboutToQuit` uses the bounded path (no unbounded `waitForStopped` /
  `wait()`).
- the Docker bookworm build still compiles and links.

Manual acceptance: live camera publish to the relay, then Stop - UI returns to
idle in ~1-2 s and the panel shows "Stopped by user"; repeat several times
including a mid-stall stop. Temporary `[moqxr][stop]` stderr tracing may be used
to time it during development, then removed before the final commit.

Honest limitation: the moq2ts-side bounded-join/detach logic is not unit-tested
(no harness), only structurally guarded + manually verified - the same coverage
level as the rest of this app. The genuinely tricky logic (the cancellation race)
lives in moqxr, where it is properly unit-tested.

## Files

moqxr:
- `include/openmoq/publisher/transport/moqt_session.h` - add `std::atomic<bool>
  stop_requested_`, `<atomic>` include.
- `src/transport/moqt_session.cpp` - set flag in `close()` (A2); observe it in the
  publish loop (A3); bound the post-loop flush (A4); stop-vs-failure return (A5).
- `tests/moqt_session_test.cpp` - Tests 1-4.

moq2ts:
- `src/media/LivePipeline.cpp` / `.h` - bounded join + detach fallback (B1),
  worker-completion signal (B2).
- `src/main.cpp` - bounded `aboutToQuit` teardown (B3).
- `tests/stop-shutdown-guards.sh` - structural guards.

## Out of scope (recorded)

- The relay-side playback / ProStream-promotion issue (no audio track / TSMuxer
  waiting for `audioPid`) - separate investigation.
- The ~60 s relay inactivity close (a relay-side policy; the bounded flush makes
  it harmless on our side).
- Replacing the app-side bash-guard test approach with a C++ unit harness.

## Verification

- moqxr: Tests 1-4 pass (Test 1 specifically would hang against pre-fix code).
- moq2ts: `tests/stop-shutdown-guards.sh` passes; Docker bookworm build links.
- Manual: repeated Stop (including mid-stall) returns the UI to idle in ~1-2 s
  with "Stopped by user"; no force-close needed.
