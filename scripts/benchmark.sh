#!/usr/bin/env bash
#
# SwiftLink benchmark driver.
#
# Emits a markdown table of individual runs -- never an average standing in for
# runs that were not performed. Every run is verified by SHA-256 against the
# source; a run that did not reproduce the file is reported as FAILED and its
# timing is not presented as a result, because a throughput number from a
# transfer that corrupted the file is worse than no number at all.
#
# Usage:
#   scripts/benchmark.sh [options]
#     --size=BYTES      test file size            (default 10485760 = 10 MiB)
#     --runs=N          runs per condition        (default 3)
#     --windows=LIST    comma-separated           (default 1,8,32)
#     --loss=LIST       comma-separated           (default 0.00,0.01,0.05)
#     --delay-us=N      one-way delay via delay_proxy; RTT is 2N
#                       (default 0, meaning no proxy in the path)
#     --build=DIR       build directory
#
# The --delay-us path exists because this development kernel is compiled
# without netem (CONFIG_NET_SCH_NETEM is not set), so `tc qdisc ... netem
# delay` is unavailable at any privilege level. See docs/benchmarks.md.

set -u

BUILD="$(cd "$(dirname "$0")/.." && pwd)/build"
SIZE=10485760
RUNS=3
WINDOWS="1,8,32"
LOSSES="0.00,0.01,0.05"
DELAY_US=0

for arg in "$@"; do
  case "$arg" in
    --size=*)     SIZE="${arg#*=}" ;;
    --runs=*)     RUNS="${arg#*=}" ;;
    --windows=*)  WINDOWS="${arg#*=}" ;;
    --loss=*)     LOSSES="${arg#*=}" ;;
    --delay-us=*) DELAY_US="${arg#*=}" ;;
    --build=*)    BUILD="${arg#*=}" ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

CLIENT="$BUILD/swiftlink_client"
SERVER="$BUILD/swiftlink_server"
PROXY="$BUILD/delay_proxy"

for binary in "$CLIENT" "$SERVER"; do
  [ -x "$binary" ] || { echo "missing $binary (build first)" >&2; exit 2; }
done
if [ "$DELAY_US" -gt 0 ] && [ ! -x "$PROXY" ]; then
  echo "missing $PROXY (needed for --delay-us)" >&2; exit 2
fi

WORK="$(mktemp -d)"
SRV_PID=""
PRX_PID=""
cleanup() {
  [ -n "$SRV_PID" ] && kill -INT "$SRV_PID" 2>/dev/null
  [ -n "$PRX_PID" ] && kill "$PRX_PID" 2>/dev/null
  wait 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

SRC="$WORK/bench.bin"
DST="$WORK/out"
mkdir -p "$DST"
head -c "$SIZE" /dev/urandom > "$SRC"
SRC_SHA=$(sha256sum "$SRC" | cut -d' ' -f1)

SERVER_PORT=$(( 20000 + (RANDOM % 20000) ))
PROXY_PORT=$(( SERVER_PORT + 1 ))

"$SERVER" "$SERVER_PORT" "$DST" --quiet --idle-ms=120000 > "$WORK/server.log" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 200); do grep -q READY "$WORK/server.log" 2>/dev/null && break; sleep 0.05; done

TARGET_PORT="$SERVER_PORT"
if [ "$DELAY_US" -gt 0 ]; then
  "$PROXY" "$PROXY_PORT" "$SERVER_PORT" "$DELAY_US" 600000 > "$WORK/proxy.log" 2>&1 &
  PRX_PID=$!
  for _ in $(seq 1 200); do grep -q READY "$WORK/proxy.log" 2>/dev/null && break; sleep 0.05; done
  TARGET_PORT="$PROXY_PORT"
fi

echo "# SwiftLink benchmark"
echo
echo "- file: $SIZE bytes, sha256 \`$SRC_SHA\`"
echo "- runs per condition: $RUNS"
if [ "$DELAY_US" -gt 0 ]; then
  echo "- path: client -> delay_proxy (${DELAY_US}us one way) -> server"
else
  echo "- path: loopback, no artificial delay"
fi
echo "- $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //'), $(uname -sr)"
echo
echo "| window | loss | run | elapsed s | throughput Mbps | packets sent | retransmissions | verify |"
echo "|---|---|---|---|---|---|---|---|"

IFS=',' read -ra WINDOW_LIST <<< "$WINDOWS"
IFS=',' read -ra LOSS_LIST <<< "$LOSSES"

for window in "${WINDOW_LIST[@]}"; do
  for loss in "${LOSS_LIST[@]}"; do
    for run in $(seq 1 "$RUNS"); do
      rm -f "$DST/bench.bin"
      "$CLIENT" 127.0.0.1 "$TARGET_PORT" "$SRC" \
        --window="$window" --simulate-loss="$loss" --quiet \
        > "$WORK/client.log" 2>&1
      rc=$?

      stats=$(grep '^STATS' "$WORK/client.log")
      elapsed=$(sed -n 's/.*elapsed_s=\([0-9.]*\).*/\1/p' <<<"$stats")
      mbps=$(sed -n 's/.*throughput_mbps=\([0-9.]*\).*/\1/p' <<<"$stats")
      sent=$(sed -n 's/.*packets_sent=\([0-9]*\).*/\1/p' <<<"$stats")
      retx=$(sed -n 's/.*retransmissions=\([0-9]*\).*/\1/p' <<<"$stats")

      out_sha=$(sha256sum "$DST/bench.bin" 2>/dev/null | cut -d' ' -f1)
      if [ "$rc" -ne 0 ] || [ "$out_sha" != "$SRC_SHA" ]; then
        verify="**FAILED**"
        elapsed="${elapsed:--}"; mbps="-"; sent="${sent:--}"; retx="${retx:--}"
      else
        verify="ok"
      fi

      echo "| $window | $loss | $run | $elapsed | $mbps | $sent | $retx | $verify |"
    done
  done
done

echo
echo "Generated $(date -u '+%Y-%m-%d %H:%M UTC')"
