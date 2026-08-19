#!/usr/bin/env bash
#
# How many simultaneous transfers does the single-threaded epoll server hold?
#
# The question is answered by ramping until it breaks, not by asserting a
# number. Each step launches N clients at once against one server and verifies
# all N files by SHA-256; the reported figure is the largest N at which every
# transfer completed and every file matched.
#
# WHY EVERY CLIENT NEEDS ITS OWN --name
# -------------------------------------
# The receiver derives the output path from the filename in the START packet.
# Two concurrent transfers advertising the same name open the same file and
# interleave their writes, so a run without distinct names measures that
# collision (documented in docs/todo.md) rather than concurrency. Each client
# here sends a unique name, so a failure is about the server's capacity to
# multiplex, which is the thing under test.
#
# WHY NO PROXY
# ------------
# tools/delay_proxy tracks one client address, so impairment and concurrency
# cannot be combined. This runs over plain loopback: the limit being looked for
# is the server's, and loopback is the configuration that reaches it soonest
# without a network in the way.
#
# Usage:
#   scripts/concurrency_test.sh [options]
#     --levels="1 2 4 ..."  client counts to try   (default 1 2 4 8 16 32 64)
#     --size=BYTES          per-client file size   (default 1048576 = 1 MiB)
#     --window=N            sender window          (default 32)
#     --rto-ms=N            retransmit timeout     (default 300)
#     --max-sessions=N      server admission limit (default 1024, so the ramp
#                           measures the machine rather than the limit)
#     --build=DIR           build directory        (default build-release)

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-release"
LEVELS="1 2 4 8 16 32 64"
SIZE=1048576
WINDOW=32
RTO_MS=300
# Deliberately above any level this ramp tries. The server's own default is 256
# (see ServerConfig::max_sessions); leaving it there would make the ramp
# rediscover that constant instead of finding where the machine gives out.
MAX_SESSIONS=1024

for arg in "$@"; do
  case "$arg" in
    --levels=*) LEVELS="${arg#*=}" ;;
    --size=*)   SIZE="${arg#*=}" ;;
    --window=*) WINDOW="${arg#*=}" ;;
    --rto-ms=*) RTO_MS="${arg#*=}" ;;
    --max-sessions=*) MAX_SESSIONS="${arg#*=}" ;;
    --build=*)  BUILD="${arg#*=}" ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

CLIENT="$BUILD/swiftlink_client"
SERVER="$BUILD/swiftlink_server"
for binary in "$CLIENT" "$SERVER"; do
  [ -x "$binary" ] || { echo "missing $binary (build first)" >&2; exit 2; }
done

WORK="$(mktemp -d)"
SRV_PID=""
cleanup() {
  [ -n "$SRV_PID" ] && kill -INT "$SRV_PID" 2>/dev/null
  wait 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

SRC="$WORK/payload.bin"
head -c "$SIZE" /dev/urandom > "$SRC"
SRC_SHA=$(sha256sum "$SRC" | cut -d' ' -f1)

echo "# SwiftLink concurrency ceiling"
echo
echo "- one server process, one thread, one epoll loop, one UDP socket"
echo "- $SIZE bytes per client, window $WINDOW, rto ${RTO_MS}ms, loopback, no injected loss"
echo "- every client advertises a distinct filename (see the script header)"
echo "- verification: SHA-256 of every received file against the source"
echo "- $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //'), $(uname -sr)"
echo "- \`net.core.rmem_max\` = $(cat /proc/sys/net/core/rmem_max), \`ulimit -n\` = $(ulimit -n)"
echo "- server \`--max-sessions=$MAX_SESSIONS\` (above every level tried, so the limit under test is the machine's)"
echo "- build: $BUILD"
echo
echo "| clients | completed | sha256 ok | wall s | aggregate Mbps | retransmits | server duplicates | refused |"
echo "|---|---|---|---|---|---|---|---|"

BEST=0
PORT=$(( 21000 + (RANDOM % 15000) ))

for n in $LEVELS; do
  PORT=$(( PORT + 1 ))
  DST="$WORK/dst$n"
  mkdir -p "$DST"

  "$SERVER" "$PORT" "$DST" --idle-ms=180000 --max-sessions="$MAX_SESSIONS" \
    > "$WORK/server.$n.log" 2>&1 &
  SRV_PID=$!
  for _ in $(seq 1 200); do
    grep -q READY "$WORK/server.$n.log" 2>/dev/null && break
    sleep 0.05
  done

  start=$(date +%s.%N)
  pids=""
  for i in $(seq 1 "$n"); do
    timeout 600 "$CLIENT" 127.0.0.1 "$PORT" "$SRC" --quiet \
      --window="$WINDOW" --rto-ms="$RTO_MS" --name="c$i.bin" \
      > "$WORK/client.$n.$i.log" 2>&1 &
    pids="$pids $!"
  done

  completed=0
  for p in $pids; do
    if wait "$p"; then completed=$(( completed + 1 )); fi
  done
  end=$(date +%s.%N)
  wall=$(awk "BEGIN{printf \"%.3f\", $end - $start}")

  ok=0
  for i in $(seq 1 "$n"); do
    got=$(sha256sum "$DST/c$i.bin" 2>/dev/null | cut -d' ' -f1)
    [ "$got" = "$SRC_SHA" ] && ok=$(( ok + 1 ))
  done

  retx=$(grep -ho 'retransmissions=[0-9]*' "$WORK"/client.$n.*.log 2>/dev/null \
         | cut -d= -f2 | awk '{s+=$1} END{print s+0}')
  refused=$(sed -n 's/.*, \([0-9]*\) refused/\1/p' "$WORK/server.$n.log")
  [ -n "$refused" ] || refused=0
  dups=$(grep -ho '[0-9]* duplicates' "$WORK/server.$n.log" 2>/dev/null \
         | cut -d' ' -f1 | awk '{s+=$1} END{print s+0}')

  kill -INT "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=""

  mbps=$(awk "BEGIN{printf \"%.2f\", ($SIZE * $ok * 8) / $wall / 1e6}")

  flag=""
  if [ "$completed" -eq "$n" ] && [ "$ok" -eq "$n" ]; then
    BEST=$n
  else
    flag=" **"
  fi

  printf '| %d | %d/%d%s | %d/%d | %s | %s | %s | %s | %s |\n' \
    "$n" "$completed" "$n" "$flag" "$ok" "$n" "$wall" "$mbps" "$retx" "$dups" \
    "$refused"

  rm -rf "$DST"
done

echo
echo "## Result"
echo
echo "- **$BEST concurrent transfers** completed with every file verified byte-identical."
echo "- Rows marked \`**\` had at least one client fail or one file mismatch."
echo
echo "Generated $(date -u '+%Y-%m-%d %H:%M UTC')"
