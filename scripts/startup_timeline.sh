#!/usr/bin/env bash
# Time Nightcap's startup and print a per-phase timeline.
#
# Run Nightcap with MOCKTAIL_STARTUP_TRACE so each phase boundary reports its
# own cost, exit shortly after the first presented frame, and repeat a few times.
# The per-phase numbers move with machine load, so the median is what gets
# printed; a single run tells you which phase is large, not how large.
#
# This is a measurement tool, not a gate: it launches the real client against
# the real payload, opens a window, and needs a display.

set -euo pipefail
umask 077

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

RUNS="${MOCKTAIL_TIMELINE_RUNS:-3}"
LOG_DIR="${MOCKTAIL_LOG_DIR:-${ROOT}/logs/startup_timeline}"
AUTO_EXIT_MS="${MOCKTAIL_TIMELINE_AUTO_EXIT_MS:-1500}"

if [[ ! -x "${ROOT}/build/mocktail" ]]; then
  echo "missing binary: ${ROOT}/build/mocktail" >&2
  echo "run 'make build' first" >&2
  exit 2
fi
if ! [[ "${RUNS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "MOCKTAIL_TIMELINE_RUNS must be a positive integer" >&2
  exit 2
fi

mkdir -p "${LOG_DIR}"

# Warm the page cache so the first measured run is not the only one that pays to
# read a 118 MiB payload off disk. Without this the payload-hash phase reads as
# a disk cost on run 1 and a CPU cost on the rest, which is not the comparison
# anyone wants.
echo "warming payload page cache..."
cat "${ROOT}/rbx_bin/libroblox.so" > /dev/null 2>&1 || true

LOGS=()
for ((run = 1; run <= RUNS; ++run)); do
  log="${LOG_DIR}/run${run}.log"
  echo "run ${run}/${RUNS} -> ${log}"
  if (( run > 1 )); then
    # Nightcap takes a per-user instance lock. Exiting releases it, but not
    # instantly, and a launch that races it fails with "already running" rather
    # than waiting -- which would look like a missing startup timeline. Wait for
    # the lock file itself to go away instead of sleeping a guessed interval.
    lock="${XDG_STATE_HOME:-${HOME}/.local/state}/mocktail/instance.lock"
    for _ in $(seq 1 100); do
      [[ -e "${lock}" ]] || break
      sleep 0.1
    done
    sleep 0.3
  fi
  set +e
  MOCKTAIL_STARTUP_TRACE=1 \
    MOCKTAIL_UPDATE_TRACE=1 \
    MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS="${AUTO_EXIT_MS}" \
    timeout --kill-after=5s 120 "${ROOT}/build/mocktail" \
      > "${log}" 2>&1
  status=$?
  set -e
  if (( status != 0 )); then
    echo "run ${run} exited with ${status}; see ${log}" >&2
  fi
  if ! grep -q "\[startup\]" "${log}"; then
    echo "run ${run} produced no [startup] lines; see ${log}" >&2
    echo "(is this build/mocktail newer than the instrumentation?)" >&2
    exit 1
  fi
  LOGS+=("${log}")
done

# Phase -> the "total" column, i.e. time since process start at each boundary.
# That is what makes a gap readable: the difference between two consecutive
# totals is what that phase cost.
python3 - "${LOGS[@]}" <<'PYTHON'
import collections
import re
import statistics
import sys

LINE = re.compile(r"\[startup\]\s+(.*?)\s+(\d+)\s+ms\s+\(total\s+(\d+)\s+ms\)")

per_run = []
for path in sys.argv[1:]:
    phases = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            match = LINE.search(raw)
            if match:
                phases.append((match.group(1), int(match.group(2)),
                               int(match.group(3))))
    if phases:
        per_run.append(phases)

if not per_run:
    sys.exit("no [startup] lines were found in any run")

print()
print(f"{'phase':<40} {'median':>9} {'min':>7} {'max':>7}  {'at':>8}")
print("-" * 78)

# The union of phases, in the order the first run reported them.
ordered = []
for name, _, _ in per_run[0]:
    if name not in ordered:
        ordered.append(name)

for name in ordered:
    totals = [
        total
        for phases in per_run
        for phase, _, total in phases
        if phase == name
    ]
    if not totals:
        continue
    deltas = [delta for phases in per_run for phase, delta, _ in phases
              if phase == name]
    print(f"{name:<40} {statistics.median(deltas):>7.0f}ms "
          f"{min(deltas):>5.0f}ms {max(deltas):>5.0f}ms "
          f"{statistics.median(totals):>6.0f}ms")

# Time to first frame is the total on the present mark, and it is the number the
# rest of the table exists to explain.
frames = [
    total
    for phases in per_run
    for name, _, total in phases
    if name == "first-present"
]
if frames:
    print("-" * 78)
    print(f"{'time to first frame':<40} {statistics.median(frames):>7.0f}ms "
          f"{min(frames):>5.0f}ms {max(frames):>5.0f}ms")

missing = len(per_run) != len(sys.argv) - 1
if missing:
    print()
    print("note: some runs produced no [startup] lines and are excluded")
PYTHON