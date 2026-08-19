#!/usr/bin/env bash
#
# Correctness harness for the reliability layer, under an impaired path.
#
# The question this answers is not "how fast is it" but "does the thing we
# built on top of UDP actually deliver the file". Every condition ends in a
# SHA-256 comparison of source against what landed on disk. A transfer that
# completed but produced different bytes is a FAIL, and so is a transfer that
# did not complete -- the point of a reliable transport is that neither
# happens.
#
# WHY NOT tc netem
# ----------------
# This kernel is built without it (`# CONFIG_NET_SCH_NETEM is not set`) and has
# no sch_netem module on disk, so netem is unavailable at any privilege level.
# A veth pair in a netns does not help; it works around loopback traversing a
# qdisc twice, not a missing qdisc. tools/delay_proxy.cpp reimplements netem's
# models in userspace and sits in the data path instead. See its header for
# where it is and is not equivalent.
#
# EVERY CONDITION PROVES ITSELF
# -----------------------------
# The proxy reports what it actually did, per direction, and those counters go
# in the table next to the verdict. A row claiming 5% loss whose proxy stats
# show zero drops did not test what it claims, and the table shows that rather
# than hiding it.
#
# Usage:
#   scripts/impair_test.sh [options]
#     --trials=N     runs per condition          (default 3)
#     --size=BYTES   test file size              (default 1048576 = 1 MiB)
#     --delay-us=N   one-way delay; RTT is 2N    (default 5000 = 10ms RTT)
#     --rto-ms=N     client retransmit timeout   (default 100)
#     --window=N     sender window               (default 32)
#     --suite=NAME   impair | boundary | all     (default all)
#     --build=DIR    build directory             (default build-release)

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-release"
TRIALS=3
SIZE=1048576
DELAY_US=5000
RTO_MS=100
WINDOW=32
SUITE=all

for arg in "$@"; do
  case "$arg" in
    --trials=*)  TRIALS="${arg#*=}" ;;
    --size=*)    SIZE="${arg#*=}" ;;
    --delay-us=*) DELAY_US="${arg#*=}" ;;
    --rto-ms=*)  RTO_MS="${arg#*=}" ;;
    --window=*)  WINDOW="${arg#*=}" ;;
    --suite=*)   SUITE="${arg#*=}" ;;
    --build=*)   BUILD="${arg#*=}" ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

CLIENT="$BUILD/swiftlink_client"
SERVER="$BUILD/swiftlink_server"
PROXY="$BUILD/delay_proxy"
for binary in "$CLIENT" "$SERVER" "$PROXY"; do
  [ -x "$binary" ] || { echo "missing $binary (build first)" >&2; exit 2; }
done

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

PASS=0
FAIL=0
PORT_BASE=$(( 24000 + (RANDOM % 15000) ))
PORT_STEP=0

# Runs one transfer end to end through the proxy and verifies the result.
#
#   run_case <label> <source-file> <proxy-options...>
#
# Emits one markdown table row per trial. Sets the globals PASS / FAIL.
run_case() {
  local label="$1"; shift
  local src="$1"; shift
  local proxy_opts=("$@")

  local src_sha
  src_sha=$(sha256sum "$src" | cut -d' ' -f1)
  local src_size
  src_size=$(stat -c%s "$src")

  local trial
  for trial in $(seq 1 "$TRIALS"); do
    PORT_STEP=$(( PORT_STEP + 2 ))
    local sport=$(( PORT_BASE + PORT_STEP ))
    local pport=$(( sport + 1 ))
    local dst="$WORK/out.$sport"
    mkdir -p "$dst"

    "$SERVER" "$sport" "$dst" --quiet --idle-ms=180000 \
      > "$WORK/server.log" 2>&1 &
    SRV_PID=$!
    local i
    for i in $(seq 1 200); do
      grep -q READY "$WORK/server.log" 2>/dev/null && break
      sleep 0.05
    done

    # A fresh seed per trial, so three trials of one condition are three
    # different loss patterns rather than the same one three times.
    "$PROXY" "$pport" "$sport" "$DELAY_US" 20000 \
      --seed="$(( 1000 + PORT_STEP + trial ))" "${proxy_opts[@]}" \
      > "$WORK/proxy.log" 2>&1 &
    PRX_PID=$!
    for i in $(seq 1 200); do
      grep -q READY "$WORK/proxy.log" 2>/dev/null && break
      sleep 0.05
    done

    local name; name="$(basename "$src")"
    timeout 300 "$CLIENT" 127.0.0.1 "$pport" "$src" \
      --window="$WINDOW" --rto-ms="$RTO_MS" --quiet \
      > "$WORK/client.log" 2>&1
    local rc=$?

    local out_sha=""
    if [ -f "$dst/$name" ]; then
      out_sha=$(sha256sum "$dst/$name" | cut -d' ' -f1)
    fi
    local out_size=0
    [ -f "$dst/$name" ] && out_size=$(stat -c%s "$dst/$name")

    # Let the proxy flush its stats line before reading it.
    kill -INT "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=""
    kill "$PRX_PID" 2>/dev/null; wait "$PRX_PID" 2>/dev/null; PRX_PID=""

    local pstats; pstats=$(grep '^PROXY_STATS' "$WORK/proxy.log" 2>/dev/null)
    local field
    field() { sed -n "s/.*$1=\([0-9]*\).*/\1/p" <<<"$pstats"; }
    local fwd_rx fwd_drop fwd_dup fwd_reo fwd_cor rev_rx rev_drop
    fwd_rx=$(field fwd_received);   fwd_drop=$(field fwd_dropped)
    fwd_dup=$(field fwd_duplicated); fwd_reo=$(field fwd_reordered)
    fwd_cor=$(field fwd_corrupted); rev_rx=$(field rev_received)
    rev_drop=$(field rev_dropped)

    local retx; retx=$(sed -n 's/.*retransmissions=\([0-9]*\).*/\1/p' \
      "$WORK/client.log")
    [ -n "$retx" ] || retx="-"

    local verdict
    if [ "$rc" -eq 0 ] && [ "$out_sha" = "$src_sha" ]; then
      verdict="PASS"; PASS=$(( PASS + 1 ))
    else
      verdict="**FAIL**"; FAIL=$(( FAIL + 1 ))
      {
        echo "### FAIL: $label trial $trial"
        echo "rc=$rc size_in=$src_size size_out=$out_size"
        echo "src_sha=$src_sha"
        echo "out_sha=${out_sha:-<no file>}"
        echo "client: $(cat "$WORK/client.log")"
        echo "server: $(tail -5 "$WORK/server.log")"
        echo "proxy:  $pstats"
      } >> "$WORK/failures.txt"
    fi

    # Drop rate the proxy actually applied, forward direction, as a check that
    # the condition was real rather than merely requested.
    local applied="-"
    if [ -n "${fwd_rx:-}" ] && [ "${fwd_rx:-0}" -gt 0 ]; then
      applied=$(awk "BEGIN{printf \"%.2f\", 100*${fwd_drop:-0}/$fwd_rx}")
    fi

    printf '| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
      "$label" "$trial" "$verdict" "${fwd_rx:--}" "${fwd_drop:--}" \
      "$applied" "${fwd_dup:--}" "${fwd_reo:--}" "${fwd_cor:--}" "$retx"

    rm -rf "$dst"
  done
}

echo "# SwiftLink correctness under an impaired path"
echo
echo "- impairment source: \`tools/delay_proxy\` (userspace; netem unavailable on this kernel)"
echo "- file: $SIZE bytes of /dev/urandom"
echo "- one-way delay ${DELAY_US}us (RTT $(awk "BEGIN{print $DELAY_US/500}")ms), rto ${RTO_MS}ms, window $WINDOW"
echo "- trials per condition: $TRIALS"
echo "- verification: SHA-256 of source vs received file, every trial"
echo "- $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //'), $(uname -sr)"
echo "- build: $BUILD"
echo

SRC="$WORK/payload.bin"
head -c "$SIZE" /dev/urandom > "$SRC"

if [ "$SUITE" = "all" ] || [ "$SUITE" = "impair" ]; then
  echo "## Impairment matrix"
  echo
  echo "\`fwd_*\` are proxy counters for the client->server direction; \`applied%\` is"
  echo "the drop rate the proxy actually delivered, which is what makes a row"
  echo "evidence rather than an assertion."
  echo
  echo "| condition | trial | result | fwd pkts | fwd dropped | applied% | fwd dup | fwd reord | fwd corrupt | client retx |"
  echo "|---|---|---|---|---|---|---|---|---|---|"

  run_case "clean (control)"        "$SRC"
  run_case "loss 1%"                "$SRC" --loss=1%
  run_case "loss 5%"                "$SRC" --loss=5%
  run_case "loss 10%"               "$SRC" --loss=10%
  run_case "loss 20%"               "$SRC" --loss=20%
  run_case "loss 10% fwd only"      "$SRC" --loss=10% --impair=to-server
  run_case "loss 10% ACKs only"     "$SRC" --loss=10% --impair=to-client
  run_case "burst 5% len4 (G-E)"    "$SRC" --burst-loss=5%  --burst-len=4
  run_case "burst 10% len8 (G-E)"   "$SRC" --burst-loss=10% --burst-len=8
  run_case "duplicate 1%"           "$SRC" --duplicate=1%
  run_case "duplicate 10%"          "$SRC" --duplicate=10%
  run_case "reorder 25%"            "$SRC" --reorder=25%
  run_case "reorder 50%"            "$SRC" --reorder=50%
  run_case "corrupt 1%"             "$SRC" --corrupt=1%
  run_case "corrupt 5%"             "$SRC" --corrupt=5%
  run_case "combined hostile"       "$SRC" --loss=5% --reorder=25% \
                                           --duplicate=2% --corrupt=1%
  echo
fi

if [ "$SUITE" = "all" ] || [ "$SUITE" = "boundary" ]; then
  echo "## Boundary file sizes"
  echo
  echo "Chunk size is 1200 bytes, so these straddle the chunking arithmetic:"
  echo "empty, sub-chunk, one byte under/over an exact chunk, exact multiples."
  echo "Each is run over a lossy *and* reordering path, because an off-by-one in"
  echo "chunk arithmetic is most likely to surface when the last chunk is short"
  echo "and arrives out of order."
  echo
  echo "| condition | trial | result | fwd pkts | fwd dropped | applied% | fwd dup | fwd reord | fwd corrupt | client retx |"
  echo "|---|---|---|---|---|---|---|---|---|---|"

  for bytes in 0 1 1199 1200 1201 2399 2400 2401; do
    f="$WORK/size_$bytes.bin"
    head -c "$bytes" /dev/urandom > "$f"
    run_case "size ${bytes}B" "$f" --loss=5% --reorder=25% --duplicate=2%
  done
  echo
fi

echo "## Result"
echo
echo "- **$PASS passed, $FAIL failed** across $(( PASS + FAIL )) transfers."
if [ "$FAIL" -gt 0 ]; then
  echo
  echo "### Failure detail"
  echo
  echo '```'
  cat "$WORK/failures.txt"
  echo '```'
fi
echo
echo "Generated $(date -u '+%Y-%m-%d %H:%M UTC')"

[ "$FAIL" -eq 0 ]
