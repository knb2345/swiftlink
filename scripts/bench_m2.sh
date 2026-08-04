#!/usr/bin/env bash
# M2 benchmark runner. Emits one pipe-delimited row per run to stdout, plus a
# verbatim log per run. Every row comes from a transfer that was verified
# byte-identical; a failed verification is reported as FAILED, never silently.
set -u

SP="$(cd "$(dirname "$0")" && pwd)"
B=/home/kabir/projects/swiftlink/build
D="$SP/data"
SRC="$D/bench10m.bin"
LOG="$SP/runs"
mkdir -p "$LOG"

CONDITION="$1"   # label for the results table
LOSS="$2"        # e.g. 0.00, 0.01, 0.05
RUNS="$3"
PORT_BASE="${4:-9500}"

SRC_SHA=$(sha256sum "$SRC" | cut -d' ' -f1)

for run in $(seq 1 "$RUNS"); do
  port=$((PORT_BASE + run))
  out="$D/recv_${CONDITION// /_}_${LOSS}_${run}.bin"
  tag="${CONDITION// /_}_loss${LOSS}_run${run}"
  slog="$LOG/$tag.server.log"
  clog="$LOG/$tag.client.log"
  rm -f "$out"

  "$B/swiftlink_server" "$port" "$out" > "$slog" 2>&1 &
  srv=$!
  for _ in $(seq 1 100); do grep -q READY "$slog" 2>/dev/null && break; sleep 0.05; done

  "$B/swiftlink_client" 127.0.0.1 "$port" "$SRC" --simulate-loss="$LOSS" > "$clog" 2>&1
  crc=$?
  wait $srv
  src=$?

  stats=$(grep '^STATS' "$clog")
  elapsed=$(sed -n 's/.*elapsed_s=\([0-9.]*\).*/\1/p' <<<"$stats")
  mbps=$(sed -n 's/.*throughput_mbps=\([0-9.]*\).*/\1/p' <<<"$stats")
  sent=$(sed -n 's/.*packets_sent=\([0-9]*\).*/\1/p' <<<"$stats")
  retx=$(sed -n 's/.*retransmissions=\([0-9]*\).*/\1/p' <<<"$stats")
  drops=$(sed -n 's/.*simulated_drops=\([0-9]*\).*/\1/p' <<<"$stats")

  out_sha=$(sha256sum "$out" 2>/dev/null | cut -d' ' -f1)
  if [ "$crc" -ne 0 ] || [ "$src" -ne 0 ] || [ "$out_sha" != "$SRC_SHA" ]; then
    verify="FAILED(client=$crc server=$src)"
  else
    verify="ok"
  fi

  echo "$CONDITION | $run | $elapsed | $mbps | $sent | $retx | $drops | $verify"
  rm -f "$out"
done
