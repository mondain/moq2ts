# Stop-hang Cancellation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make pressing Stop on a live publish wind the session down and return the UI to idle within ~1-2s instead of freezing the app.

**Architecture:** Two coordinated layers. In the moqxr library (sibling repo `../moqxr`), `MoqtSession` gains an atomic `stop_requested_` flag that `close()` sets and the `publish_live_objects` loop observes, with a time-bounded graceful flush before returning success. In the moq2ts app (this repo), `LivePipeline` replaces its unconditional worker `join()` with a bounded wait + detach fallback driven by a worker-completion signal, and `main.cpp`'s `aboutToQuit` teardown is bounded so the GUI thread can never block.

**Tech Stack:** C++20, picoquic/WebTransport transport (moqxr), Qt 6 (moq2ts), CMake + CTest (moqxr tests), Docker bookworm build + bash structural guards (moq2ts tests).

**Spec:** `docs/superpowers/specs/2026-05-31-stop-hang-cancellation-design.md`

**Repo paths (absolute):**
- moqxr: `/media/mondain/terrorbyte/workspace/github-moq/moqxr`
- moq2ts: `/media/mondain/terrorbyte/workspace/github-moq/moq2ts`

---

## File Structure

moqxr (`../moqxr`):
- `include/openmoq/publisher/transport/moqt_session.h` — add `<atomic>` include and the `stop_requested_` member.
- `src/transport/moqt_session.cpp` — set the flag in `close()`; observe it in the `publish_live_objects` loop; bound the post-loop flush.
- `tests/moqt_session_test.cpp` — add cancellation test blocks to the existing `main()` harness.

moq2ts (this repo):
- `src/media/LivePipeline.h` — add `<condition_variable>`, `<mutex>`, `<cstdio>`; add completion-signal members.
- `src/media/LivePipeline.cpp` — raise completion signal on every `runLoop` exit; bounded join + detach in `waitForStopped`; reset the done flag in `start`.
- `src/main.cpp` — bound the `aboutToQuit` teardown.
- `tests/stop-shutdown-guards.sh` — new structural guard script.

---

## Task 1: moqxr — add the stop flag and make the publish loop observe it

**Files:**
- Modify: `/media/mondain/terrorbyte/workspace/github-moq/moqxr/include/openmoq/publisher/transport/moqt_session.h`
- Modify: `/media/mondain/terrorbyte/workspace/github-moq/moqxr/src/transport/moqt_session.cpp`
- Test: `/media/mondain/terrorbyte/workspace/github-moq/moqxr/tests/moqt_session_test.cpp`

- [ ] **Step 1: Write the failing test (deterministic stop)**

Add `#include <thread>`, `#include <future>`, and `#include <atomic>` to the include block at the top of `tests/moqt_session_test.cpp` (none are present today).

Then add this test block inside `main()`, immediately before the final `return ok ? 0 : 1;`. It mirrors the connect/server-setup setup used by the existing draft-14 publish block (around line 882-892): queue a server setup read on stream 0, connect, then run the publish loop on a worker thread and cancel it via `close()` from the test thread.

```cpp
    {
        // Stop-hang regression: close() from another thread must make a running
        // publish_live_objects loop return promptly. Against pre-fix code this
        // hangs forever; the future wait_for bounds the failure.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        // auto_forward=true so the loop drives objects without a subscriber;
        // subscriber_timeout small so no path waits long.
        MoqtSession session(transport, std::string(kTestTrackNamespace),
                            /*auto_forward=*/true, /*publish_catalog=*/false,
                            /*paced=*/false, /*loop=*/false,
                            std::chrono::seconds(1));

        auto connect_status = session.connect(endpoint, tls);
        ok &= expect(connect_status.ok, "expected stop-cancel session connect to succeed");

        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "catalog"},
                         LiveTrack{.track_name = "vide_1"}};
        std::atomic<int> object_calls{0};
        source.next_object = [&object_calls]() -> std::optional<LiveObject> {
            const int n = object_calls.fetch_add(1);
            if (n == 0) {
                return LiveObject{.track_name = "catalog", .group_id = 0, .object_id = 0,
                                  .payload = {'I', 'N', 'I', 'T'}};
            }
            // Endless media objects so the loop never reaches natural EOF.
            return LiveObject{.track_name = "vide_1", .group_id = 1,
                              .object_id = static_cast<std::size_t>(n),
                              .payload = {'M', 'S', 'G'}};
        };

        std::promise<TransportStatus> result_promise;
        auto result_future = result_promise.get_future();
        std::thread worker([&]() {
            result_promise.set_value(session.publish_live_objects(source, DraftVersion::kDraft16));
        });
        // Let the loop spin a few iterations.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        session.close(0);

        const auto wait_status = result_future.wait_for(std::chrono::seconds(3));
        ok &= expect(wait_status == std::future_status::ready,
                     "expected publish_live_objects to return within 3s after close()");
        if (wait_status == std::future_status::ready) {
            worker.join();
            ok &= expect(result_future.get().ok,
                         "expected stop-initiated publish_live_objects to return success");
        } else {
            worker.detach();  // broken build: leak rather than hang the whole suite
        }
    }
```

- [ ] **Step 2: Build the tests and verify the new block fails (hangs → times out)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_BUILD_TESTS=ON
cmake --build build --target openmoq-publisher-transport-tests
./build/openmoq-publisher-transport-tests
```
Expected: the new block prints `FAIL: expected publish_live_objects to return within 3s after close()` after ~3s, and the binary exits non-zero. (Pre-fix, the loop never observes the close, so the future is never ready.)

- [ ] **Step 3: Add the `stop_requested_` member to the header**

In `include/openmoq/publisher/transport/moqt_session.h`, add `#include <atomic>` to the include list (after `#include <chrono>`), and add the member to the private block (after `PublishStats publish_stats_{};` near line 77):

```cpp
    std::atomic<bool> stop_requested_{false};
```

- [ ] **Step 4: Set the flag first in `close()`**

In `src/transport/moqt_session.cpp`, `MoqtSession::close` (starts at line 4225). Make the first statement set the flag, before the existing stream-reset work:

```cpp
TransportStatus MoqtSession::close(std::uint64_t application_error_code) {
    stop_requested_.store(true, std::memory_order_release);
    if (namespace_stream_open_) {
        transport_.reset_stream(namespace_stream_id_, 0x0);
        namespace_stream_id_ = 0;
        namespace_stream_open_ = false;
    }
    // ... rest unchanged ...
```

- [ ] **Step 5: Observe the flag at the top of the publish loop**

In `src/transport/moqt_session.cpp`, the `publish_live_objects` loop begins at line 4059 with `while (!source_eof) {`. Insert a check as the first statement inside the loop:

```cpp
    while (!source_eof) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }
        status = process_control_messages();
        if (!status.ok) {
            return status;
        }
```

- [ ] **Step 6: Observe the flag after the blocking read and after each send**

Still inside the loop, after the `read_stream` result handling block (the `} else { return read_status; }` that ends at line 4084), add a check before the second `process_control_messages()` call:

```cpp
        } else {
            return read_status;
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }

        status = process_control_messages();
```

Then after the catalog `serve()` success path — the `live_object_catalog_sent = true; continue;` block (around line 4135-4136) — and after the media `record_published_object(...)` call at the end of the loop body (around line 4176-4178), add the same check so a stop during/after a send breaks promptly. After the media send, it looks like:

```cpp
        record_published_object(next->track_name,
                                static_cast<std::uint64_t>(next->group_id),
                                next->payload.size());
        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }
    }
```

For the catalog branch, change its tail from:

```cpp
            live_object_catalog_sent = true;
            continue;
        }
```
to:
```cpp
            live_object_catalog_sent = true;
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
```

These breaks fall through to the existing post-loop block (`finish_group` at 4181, `PUBLISH_DONE` at 4189), which is the graceful flush. Because the exit is via `break` (not `return`) and `stop_requested_` is set, the function reaches its normal `return write_namespace_done_for_request(...)` / success tail rather than a failure path — satisfying spec A5 (stop returns success; genuine `serve`/`read` errors with the flag unset still `return failure(...)`).

- [ ] **Step 7: Rebuild and verify the deterministic-stop test passes**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
cmake --build build --target openmoq-publisher-transport-tests
./build/openmoq-publisher-transport-tests
```
Expected: no `FAIL:` lines; binary exits 0. The new block now returns within milliseconds of `close()`.

- [ ] **Step 8: Run the whole transport test suite to confirm no regressions**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
ctest --test-dir build --output-on-failure -R openmoq-publisher-transport-tests
```
Expected: `100% tests passed`.

- [ ] **Step 9: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
git add include/openmoq/publisher/transport/moqt_session.h src/transport/moqt_session.cpp tests/moqt_session_test.cpp
git commit -m "Make publish_live_objects cancellable via close() stop flag"
```

---

## Task 2: moqxr — bound the post-loop graceful flush

**Files:**
- Modify: `/media/mondain/terrorbyte/workspace/github-moq/moqxr/src/transport/moqt_session.cpp`
- Test: `/media/mondain/terrorbyte/workspace/github-moq/moqxr/tests/moqt_session_test.cpp`

Context: after Task 1, a stop breaks the loop and the post-loop flush (`finish_group` + `PUBLISH_DONE` + `write_namespace_done_for_request`) runs. On a wedged transport those writes can block. This task time-boxes the flush so the function returns even when writes never complete.

- [ ] **Step 1: Write the failing test (flush deadline fallback)**

Add this block inside `main()` in `tests/moqt_session_test.cpp`, before the final `return`. It uses a transport whose writes block until released, so the flush would hang without a deadline. Extend `MockTransport` with a blocking-write hook by adding these members to the `MockTransport` struct (after `std::function<void(MockTransport&, std::uint64_t)> on_read;` near line 249):

```cpp
    std::atomic<bool> block_writes{false};
    std::atomic<bool> release_writes{false};
```

and change `write_stream` (line 177) to honor them at its start:

```cpp
    TransportStatus write_stream(std::uint64_t stream_id,
                                 std::span<const std::uint8_t> bytes,
                                 bool fin) override {
        while (block_writes.load() && !release_writes.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        writes.push_back({
            .stream_id = stream_id,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            .fin = fin,
        });
        return TransportStatus::success();
    }
```
(Add `#include <thread>` and `#include <atomic>` if not already added in Task 1; they were.)

Test block:

```cpp
    {
        // Flush deadline: if post-stop flush writes block, publish_live_objects
        // must still return within ~deadline + slack (hard-cancel fallback).
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        MoqtSession session(transport, std::string(kTestTrackNamespace),
                            /*auto_forward=*/true, /*publish_catalog=*/false,
                            /*paced=*/false, /*loop=*/false,
                            std::chrono::seconds(1));
        auto connect_status = session.connect(endpoint, tls);
        ok &= expect(connect_status.ok, "expected flush-deadline session connect to succeed");

        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "catalog"},
                         LiveTrack{.track_name = "vide_1"}};
        std::atomic<int> object_calls{0};
        source.next_object = [&object_calls]() -> std::optional<LiveObject> {
            const int n = object_calls.fetch_add(1);
            if (n == 0) {
                return LiveObject{.track_name = "catalog", .group_id = 0, .object_id = 0,
                                  .payload = {'I', 'N', 'I', 'T'}};
            }
            return LiveObject{.track_name = "vide_1", .group_id = 1,
                              .object_id = static_cast<std::size_t>(n),
                              .payload = {'M', 'S', 'G'}};
        };

        std::promise<TransportStatus> result_promise;
        auto result_future = result_promise.get_future();
        std::thread worker([&]() {
            result_promise.set_value(session.publish_live_objects(source, DraftVersion::kDraft16));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        transport.block_writes.store(true);   // any flush write now blocks
        session.close(0);

        const auto wait_status = result_future.wait_for(std::chrono::seconds(4));
        ok &= expect(wait_status == std::future_status::ready,
                     "expected publish_live_objects to honor flush deadline and return");
        transport.release_writes.store(true);  // unblock any parked write
        if (wait_status == std::future_status::ready) {
            worker.join();
        } else {
            worker.detach();
        }
    }
```

- [ ] **Step 2: Build and verify it fails (flush blocks → times out)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
cmake --build build --target openmoq-publisher-transport-tests
./build/openmoq-publisher-transport-tests
```
Expected: `FAIL: expected publish_live_objects to honor flush deadline and return` after ~4s (no deadline yet, so the blocked flush write hangs).

- [ ] **Step 3: Add the flush deadline to the post-loop flush**

In `src/transport/moqt_session.cpp`, just after the `while (!source_eof)` loop closes (line 4179-4180, before the `for (auto& [track_name, sender] : sender_by_track)` flush loop at 4181), compute a deadline that only constrains the flush when a stop is in progress:

```cpp
    const auto flush_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const auto flush_budget_exhausted = [&]() {
        return stop_requested_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() >= flush_deadline;
    };
```

Then guard each flush write group. Before the `finish_group` loop body and each `write_*` flush call, bail out early when the budget is exhausted. The `finish_group` loop becomes:

```cpp
    for (auto& [track_name, sender] : sender_by_track) {
        static_cast<void>(track_name);
        if (flush_budget_exhausted()) {
            return TransportStatus::success();
        }
        status = sender.finish_group(transport_);
        if (!status.ok) {
            return stop_requested_.load(std::memory_order_acquire)
                       ? TransportStatus::success()
                       : status;
        }
    }
```

Apply the same pattern to the two `PUBLISH_DONE` loops (lines 4189 and 4203) and the final `write_namespace_done_for_request` (line 4218): check `flush_budget_exhausted()` first and `return TransportStatus::success();` if true; and when a write returns `!status.ok` while `stop_requested_` is set, return success instead of the failure (a flush failure during stop is expected, spec error-handling). The final statement becomes:

```cpp
    if (flush_budget_exhausted()) {
        return TransportStatus::success();
    }
    const auto namespace_done_status = write_namespace_done_for_request(transport_,
                                            draft_version,
                                            control_stream_id_,
                                            namespace_stream_id_,
                                            namespace_message);
    if (!namespace_done_status.ok && stop_requested_.load(std::memory_order_acquire)) {
        return TransportStatus::success();
    }
    return namespace_done_status;
```

- [ ] **Step 4: Rebuild and verify the flush-deadline test passes**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
cmake --build build --target openmoq-publisher-transport-tests
./build/openmoq-publisher-transport-tests
```
Expected: no `FAIL:` lines; the blocked-flush block returns within ~2s of `close()` and the binary exits 0.

- [ ] **Step 5: Add the "graceful flush happened" test (healthy transport)**

Add this block inside `main()` before the final `return`. It asserts that with a healthy transport, a stop still lets the flush emit a `PUBLISH_DONE` (control message type `0x0b`) before returning — reusing the existing `control_message_count` helper (defined near line 743):

```cpp
    {
        // Graceful flush: with a healthy transport, a stop still flushes
        // PUBLISH_DONE before returning success.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        MoqtSession session(transport, std::string(kTestTrackNamespace),
                            /*auto_forward=*/true, /*publish_catalog=*/false,
                            /*paced=*/false, /*loop=*/false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected graceful-flush session connect to succeed");

        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "catalog"},
                         LiveTrack{.track_name = "vide_1"}};
        std::atomic<int> object_calls{0};
        source.next_object = [&object_calls]() -> std::optional<LiveObject> {
            const int n = object_calls.fetch_add(1);
            if (n == 0) {
                return LiveObject{.track_name = "catalog", .group_id = 0, .object_id = 0,
                                  .payload = {'I', 'N', 'I', 'T'}};
            }
            return LiveObject{.track_name = "vide_1", .group_id = 1,
                              .object_id = static_cast<std::size_t>(n),
                              .payload = {'M', 'S', 'G'}};
        };

        std::promise<TransportStatus> result_promise;
        auto result_future = result_promise.get_future();
        std::thread worker([&]() {
            result_promise.set_value(session.publish_live_objects(source, DraftVersion::kDraft16));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        session.close(0);
        const auto wait_status = result_future.wait_for(std::chrono::seconds(3));
        ok &= expect(wait_status == std::future_status::ready,
                     "expected graceful-flush publish to return after close()");
        if (wait_status == std::future_status::ready) {
            worker.join();
            ok &= expect(result_future.get().ok, "expected graceful-flush return success");
            ok &= expect(control_message_count(transport, 0x0b) >= 1,
                         "expected at least one PUBLISH_DONE during graceful flush");
        } else {
            worker.detach();
        }
    }
```

- [ ] **Step 6: Rebuild and run the full transport suite**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
cmake --build build --target openmoq-publisher-transport-tests
ctest --test-dir build --output-on-failure -R openmoq-publisher-transport-tests
```
Expected: `100% tests passed`. (If the graceful-flush block's `PUBLISH_DONE` assertion proves environment-dependent because auto_forward emits namespace-done rather than per-subscription PUBLISH_DONE, relax it to assert `transport.writes.size() >= 2` — i.e. at least the catalog object plus one flush write — and keep the success assertion. Note this adjustment in the commit message.)

- [ ] **Step 7: Add the "no false positive" test (genuine error still fails)**

Add this block inside `main()` before the final `return`. It confirms that a transport error with `stop_requested_` UNSET still propagates as `failure(...)` — i.e. the cancellation change did not swallow real errors (spec A5 boundary). It uses `missing_read_error` (an existing `MockTransport` field, line 243) to force `read_stream` to fail, without ever calling `close()`.

```cpp
    {
        // No false positive: a transport error WITHOUT a stop request must still
        // return failure, not success.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        transport.missing_read_error = "synthetic fatal read error";
        MoqtSession session(transport, std::string(kTestTrackNamespace),
                            /*auto_forward=*/true, /*publish_catalog=*/false,
                            /*paced=*/false, /*loop=*/false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected no-false-positive session connect to succeed");

        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "catalog"},
                         LiveTrack{.track_name = "vide_1"}};
        source.next_object = []() -> std::optional<LiveObject> {
            return LiveObject{.track_name = "catalog", .group_id = 0, .object_id = 0,
                              .payload = {'I', 'N', 'I', 'T'}};
        };

        // No close() is ever called here; the read error is the only exit.
        const TransportStatus result = session.publish_live_objects(source, DraftVersion::kDraft16);
        ok &= expect(!result.ok,
                     "expected a transport read error without stop to return failure");
    }
```

Note: this assumes the loop's `read_stream` runs before any object is sent in auto_forward mode. If in practice the catalog object is sent first and the loop exits cleanly, instead drive the failure through `serve()` by leaving `missing_read_error` unset and making `next_object` return an object whose `track_name` is not in the source tracks (the loop returns `failure("live object references unknown track: ...")` at `moqt_session.cpp:4105`). Either path validates that a non-stop error returns `failure`; keep whichever compiles and reproduces a failing status.

- [ ] **Step 8: Rebuild and run the full transport suite**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
cmake --build build --target openmoq-publisher-transport-tests
ctest --test-dir build --output-on-failure -R openmoq-publisher-transport-tests
```
Expected: `100% tests passed`.

- [ ] **Step 9: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr
git add src/transport/moqt_session.cpp tests/moqt_session_test.cpp
git commit -m "Bound the publish_live_objects graceful flush on stop"
```

---

## Task 3: moq2ts — bounded worker join with detach fallback

**Files:**
- Modify: `/media/mondain/terrorbyte/workspace/github-moq/moq2ts/src/media/LivePipeline.h`
- Modify: `/media/mondain/terrorbyte/workspace/github-moq/moq2ts/src/media/LivePipeline.cpp`

Context: today `waitForStopped()` does an unconditional `m_workerThread.join()` (`LivePipeline.cpp:105-109`). The worker can be wedged; this task bounds the wait and detaches as a last resort, driven by a completion signal the worker raises on every exit path. This repo has no C++ unit harness — verification is the Docker build (Task 5) plus the guard script.

- [ ] **Step 1: Add completion-signal members and includes to the header**

In `src/media/LivePipeline.h`, add includes near the existing `#include <atomic>` / `#include <thread>` (top of file):

```cpp
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
```

Add these members to the private block (after `std::thread m_workerThread;` at line 41):

```cpp
    std::mutex m_doneMutex;
    std::condition_variable m_doneCv;
    bool m_workerDone = false;
```

- [ ] **Step 2: Reset the done flag when starting**

In `src/media/LivePipeline.cpp`, `LivePipeline::start` (line 60). Immediately before the worker thread is launched (line 87 `m_workerThread = std::thread(...)`), reset the flag under the lock:

```cpp
    {
        std::lock_guard<std::mutex> lock(m_doneMutex);
        m_workerDone = false;
    }
    m_workerThread = std::thread([this]() { runLoop(); });
```

- [ ] **Step 3: Raise the completion signal on every `runLoop` exit**

In `src/media/LivePipeline.cpp`, `runLoop` (line 111) has multiple `return` paths. Add a RAII guard as the very first statement of `runLoop` so the signal fires on all of them. Add `#include <cstdio>` to the include block at the top of the file (after `#include <chrono>`), then:

```cpp
void LivePipeline::runLoop() {
    struct DoneSignal {
        LivePipeline* self;
        ~DoneSignal() {
            {
                std::lock_guard<std::mutex> lock(self->m_doneMutex);
                self->m_workerDone = true;
            }
            self->m_doneCv.notify_all();
        }
    } doneSignal{this};

    if (!m_publisher) {
        // ... existing body unchanged ...
```

- [ ] **Step 4: Replace the unconditional join with a bounded wait + detach**

In `src/media/LivePipeline.cpp`, replace `waitForStopped` (lines 105-109):

```cpp
void LivePipeline::waitForStopped() {
    if (!m_workerThread.joinable()) {
        return;
    }
    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(m_doneMutex);
        finished = m_doneCv.wait_for(lock, std::chrono::seconds(3),
                                     [this]() { return m_workerDone; });
    }
    if (finished) {
        m_workerThread.join();
    } else {
        std::fprintf(stderr, "[moqxr][stop] worker join timed out after 3000ms; detaching\n");
        std::fflush(stderr);
        m_workerThread.detach();
    }
}
```

This is idempotent: after a detach, `joinable()` is false, so a second call (e.g. from the destructor) returns immediately.

- [ ] **Step 5: Build (compile check via Docker bookworm)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: ends with `Build complete. Launcher: ...moq2ts-publisher-bookworm` and exit code 0.

- [ ] **Step 6: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/media/LivePipeline.h src/media/LivePipeline.cpp
git commit -m "Bound LivePipeline worker shutdown with detach fallback"
```

---

## Task 4: moq2ts — bound the aboutToQuit teardown

**Files:**
- Modify: `/media/mondain/terrorbyte/workspace/github-moq/moq2ts/src/main.cpp`

Context: `aboutToQuit` (lines 54-61) currently calls `pipeline.waitForStopped()` on the GUI thread and then `shutdownFuture.wait()`. After Task 3, `waitForStopped()` is already bounded, so the GUI thread can no longer hang there. This task makes the intent explicit and bounds the `shutdownFuture.wait()` too, so neither call can block unbounded.

- [ ] **Step 1: Bound the shutdownFuture wait in aboutToQuit**

In `src/main.cpp`, replace the `aboutToQuit` handler body (lines 54-61):

```cpp
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        publisher.stop();
        pipeline.requestStop();
        pipeline.waitForStopped();  // bounded (3s) + detach fallback
        if (shutdownFuture.valid()) {
            shutdownFuture.wait_for(std::chrono::seconds(3));
        }
    });
```

`<chrono>` is already included in `main.cpp` (line 5). `pipeline.waitForStopped()` is bounded by Task 3; bounding the future wait closes the last unbounded GUI-thread wait.

- [ ] **Step 2: Build (compile check via Docker bookworm)**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: `Build complete. ...` and exit code 0.

- [ ] **Step 3: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add src/main.cpp
git commit -m "Bound GUI-thread teardown at app quit"
```

---

## Task 5: moq2ts — structural guard script

**Files:**
- Create: `/media/mondain/terrorbyte/workspace/github-moq/moq2ts/tests/stop-shutdown-guards.sh`

Context: this repo's "tests" are bash scripts that grep for structural invariants so a refactor can't silently revert a fix, plus the Docker build as the compile/link check. Match the style of existing scripts like `tests/keyframe-aligned-groups-guards.sh` (each assertion greps a file and exits non-zero with a message on failure).

- [ ] **Step 1: Inspect an existing guard script for the exact style**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
sed -n '1,40p' tests/keyframe-aligned-groups-guards.sh
```
Expected: a `set -euo pipefail` header, a repo-root resolution, and a helper that greps and errors. Reuse the same structure.

- [ ] **Step 2: Write the guard script**

Create `tests/stop-shutdown-guards.sh` with the same idiom as the sibling scripts (adjust the helper name/header to match what Step 1 shows if it differs):

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0

require() {
    # require <file> <grep-pattern> <description>
    if ! grep -q "$2" "$ROOT/$1"; then
        echo "FAIL: $3 ($1 :: $2)"
        fail=1
    fi
}

# B1: bounded wait + detach fallback (not a bare join)
require "src/media/LivePipeline.cpp" "wait_for(lock, std::chrono::seconds(3)" "waitForStopped uses a bounded 3s wait"
require "src/media/LivePipeline.cpp" "m_workerThread.detach()" "waitForStopped detaches on timeout"
require "src/media/LivePipeline.cpp" "worker join timed out after 3000ms" "join-timeout warning marker present"

# B2: worker raises a completion signal on exit
require "src/media/LivePipeline.cpp" "m_workerDone = true" "runLoop raises worker-completion signal"
require "src/media/LivePipeline.h" "std::condition_variable m_doneCv" "completion condition_variable declared"

# B3: aboutToQuit teardown is bounded
require "src/main.cpp" "shutdownFuture.wait_for(std::chrono::seconds(3))" "aboutToQuit bounds the shutdown future wait"

if [ "$fail" -ne 0 ]; then
    echo "stop-shutdown-guards: FAILED"
    exit 1
fi
echo "stop-shutdown-guards: PASS"
```

- [ ] **Step 3: Make it executable and run it**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
chmod +x tests/stop-shutdown-guards.sh
bash tests/stop-shutdown-guards.sh
```
Expected: `stop-shutdown-guards: PASS`.

- [ ] **Step 4: Run the whole guard suite to confirm nothing else regressed**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
for t in tests/*.sh; do
  out=$(bash "$t" 2>&1); rc=$?
  printf "%-55s %s\n" "$(basename "$t")" "$([ $rc -eq 0 ] && echo PASS || echo "FAIL(rc=$rc)")"
done
```
Expected: every script prints `PASS`, including `stop-shutdown-guards.sh`.

- [ ] **Step 5: Commit**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts
git add tests/stop-shutdown-guards.sh
git commit -m "Add stop-shutdown structural guards"
```

---

## Task 6: Manual acceptance verification

**Files:** none (manual run).

Context: the real success criterion is the UI behavior. The unit tests cover the moqxr cancellation race; this confirms the end-to-end Stop on a live publish.

- [ ] **Step 1: Confirm both builds are current**

```bash
cd /media/mondain/terrorbyte/workspace/github-moq/moqxr && cmake --build build --target openmoq-publisher-transport-tests
cd /media/mondain/terrorbyte/workspace/github-moq/moq2ts && bash scripts/build-debian-bookworm.sh 2>&1 | tail -3
```
Expected: both succeed.

- [ ] **Step 2: Run a live publish and Stop it (ask the user to perform this)**

Have the user launch the bookworm publisher, select a camera and a microphone, publish to the relay, let it run ~10s, then press Stop. Capture stderr.

Success criteria:
- The UI returns to idle within ~1-2s and the panel shows "Stopped by user".
- No force-close needed.
- No `[moqxr][stop] worker join timed out` line in stderr (the detach fallback should not trigger; if it does, that is a defect signal to investigate, not a pass).

- [ ] **Step 3: Repeat including a mid-stall stop**

Repeat Step 2 two or three times, including pressing Stop while the relay is mid-stall (e.g. shortly after the queue backpressure the logs showed). Confirm the UI still returns to idle within the bound every time.

- [ ] **Step 4: Report results to the user**

Summarize: tests passing, build linking, and the manual Stop latency observed. If the detach fallback ever fired, report it explicitly rather than treating the run as clean.

---

## Notes for the implementer

- moqxr is a sibling git repo (`../moqxr`), on branch `main`, freely editable; commit there separately from moq2ts. The two repos are committed independently (no submodule link).
- Do not add Claude author taglines / "Generated with" / "Co-Authored-By" lines to commits (project convention).
- Prefer `import`-style usage and follow existing file conventions; do not restructure unrelated code.
- If a test block proves not to compile because of a constructor-arg or draft mismatch, mirror the connect/server-setup pattern from the existing draft-14 publish block in `tests/moqt_session_test.cpp` (around lines 882-898) and keep the cancellation assertions intact; the assertions, not the scaffolding, are the point.
