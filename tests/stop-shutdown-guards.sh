#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PIPELINE_CPP="$REPO_ROOT/src/media/LivePipeline.cpp"
PIPELINE_HDR="$REPO_ROOT/src/media/LivePipeline.h"
MAIN="$REPO_ROOT/src/main.cpp"

fail() { printf '%s\n' "$1" >&2; exit 1; }

# B1: bounded wait + detach fallback (not a bare join).
grep -q 'm_doneCv.wait_for(lock, std::chrono::seconds(3)' "$PIPELINE_CPP" \
  || fail "waitForStopped must use a bounded 3s wait on the worker-done condition"

grep -q 'm_workerThread.detach();' "$PIPELINE_CPP" \
  || fail "waitForStopped must detach the worker when the bounded wait times out"

grep -q 'worker join timed out after 3000ms' "$PIPELINE_CPP" \
  || fail "waitForStopped must log the join-timeout warning marker"

# B2: the worker raises a completion signal on every runLoop exit.
grep -q 'struct DoneSignal' "$PIPELINE_CPP" \
  || fail "runLoop must raise a worker-completion signal via RAII"

grep -q 'm_workerDone = true' "$PIPELINE_CPP" \
  || fail "runLoop must set the worker-done flag on exit"

grep -q 'std::condition_variable m_doneCv' "$PIPELINE_HDR" \
  || fail "LivePipeline must declare the worker-done condition_variable"

# B3: aboutToQuit teardown is bounded.
grep -q 'shutdownFuture.wait_for(std::chrono::seconds(3))' "$MAIN" \
  || fail "aboutToQuit must bound the shutdown future wait"

printf 'stop-shutdown guards passed\n'
