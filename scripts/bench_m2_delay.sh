#!/usr/bin/env bash
# Same as bench.sh but routes the transfer through delay_proxy so the endpoints
# see an artificial RTT. Client -> proxy(listen) -> server; replies come back
# along the same path, so RTT = 2 x one-way delay.
set -u

SP="$(cd "$(dirname "$0")" && pwd)"
B=/home/kabir/projects/swiftlink/build
D="$SP/data"
SRC="$D/bench10m.bin"
LOG="$SP/runs"
mkdir -p "$LOG"

CONDITION="$1"     # label
LOSS="$2"
RUNS="$3"
ONEWAY_US="$4"     # one-way delay in microseconds
PORT_BASE="${5:-9600}"

SRC_SHA=$(sha256sum "$SRC" | cut -d' ' -f1)

for run in $(seq 1 "$RUNS"); do
  sport=$((PORT_BASE + run * 2))
  pport=$((PORT_BASE + run * 2 + 1))
  out="$D/recv_delay_${LOSS}_${run}.bin"
  tag="${CONDITION// /_}_loss${LOSS}_run${run}"
  slog="$LOG/$tag.server.log"
  clog="$LOG/$tag.client.log"
  plog="$LOG/$tag.proxy.log"
  rm -f "$out"

  "$B/swiftlink_server" "$sport" "$out" --idle-ms=60000 > "$slog" 2>&1 &
  srv=$!
  for _ in $(seq 1 100); do grep -q READY "$slog" 2>/dev/null && break; sleep 0.05; done

  "$B/delay_proxy" "$pport" "$sport" "$ONEWAY_US" 20000 > "$plog" 2>&1 &
  prx=$!
  for _ in $(seq 1 100); do grep -q READY "$plog" 2>/dev/null && break; sleep 0.05; done

  # Client talks to the proxy port, not the server port.
  "$B/swiftlink_client" 127.0.0.1 "$pport" "$SRC" --simulate-loss="$LOSS" > "$clog" 2>&1
  crc=$?
  wait $srv; src=$?
  kill $prx 2>/dev/null; wait $prx 2>/dev/null

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
