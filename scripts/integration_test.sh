#!/usr/bin/env bash
#
# End-to-end integration tests: real client, real epoll server, real UDP.
#
# The unit tests prove the pieces behave; these prove a file that goes in one
# end comes out the other byte-identical. Verification is by SHA-256, never by
# size alone -- a size check would pass on a file whose chunks landed at the
# wrong offsets, which is exactly the failure mode positional writes could
# introduce.
#
# Usage: integration_test.sh [path-to-build-dir]

set -u

BUILD="${1:-$(cd "$(dirname "$0")/.." && pwd)/build}"
CLIENT="$BUILD/swiftlink_client"
SERVER="$BUILD/swiftlink_server"
PROXY="$BUILD/delay_proxy"

if [ ! -x "$CLIENT" ] || [ ! -x "$SERVER" ]; then
  echo "missing binaries in $BUILD (build first)" >&2
  exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null' EXIT

SRC="$WORK/src"
DST="$WORK/dst"
mkdir -p "$SRC" "$DST"

PORT=$(( 20000 + (RANDOM % 20000) ))
PASS=0
FAIL=0

start_server() {
  "$SERVER" "$PORT" "$DST" --quiet > "$WORK/server.log" 2>&1 &
  SRV_PID=$!
  for _ in $(seq 1 200); do
    grep -q READY "$WORK/server.log" 2>/dev/null && return 0
    sleep 0.05
  done
  echo "server failed to start" >&2
  return 1
}

stop_server() {
  if [ -n "${SRV_PID:-}" ]; then
    kill -INT "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
  fi
}

# run_case_as <name> <source-file> <expected-output-name> [client args...]
run_case_as() {
  local name="$1" file="$2" outname="$3"; shift 3
  local src="$SRC/$file" dst="$DST/$outname"

  rm -f "$dst"
  if ! "$CLIENT" 127.0.0.1 "$PORT" "$src" --quiet "$@" > "$WORK/client.log" 2>&1; then
    echo "[  FAIL  ] $name (client failed: $(tail -1 "$WORK/client.log"))"
    FAIL=$((FAIL + 1)); return
  fi
  local a b
  a=$(sha256sum "$src" | cut -d' ' -f1)
  b=$(sha256sum "$dst" 2>/dev/null | cut -d' ' -f1)
  if [ "$a" != "$b" ]; then
    echo "[  FAIL  ] $name (expected $dst to match source)"
    FAIL=$((FAIL + 1)); return
  fi
  echo "[  PASS  ] $name (landed as $outname)"
  PASS=$((PASS + 1))
}

# run_case <name> <filename> [extra client args...]
run_case() {
  local name="$1" file="$2"; shift 2
  local src="$SRC/$file" dst="$DST/$file"

  rm -f "$dst"
  if ! "$CLIENT" 127.0.0.1 "$PORT" "$src" --quiet "$@" > "$WORK/client.log" 2>&1; then
    echo "[  FAIL  ] $name (client exited $?)"
    sed 's/^/            /' "$WORK/client.log"
    FAIL=$((FAIL + 1)); return
  fi

  if [ ! -f "$dst" ]; then
    echo "[  FAIL  ] $name (no output file)"
    FAIL=$((FAIL + 1)); return
  fi

  local a b
  a=$(sha256sum "$src" | cut -d' ' -f1)
  b=$(sha256sum "$dst" | cut -d' ' -f1)
  if [ "$a" != "$b" ]; then
    echo "[  FAIL  ] $name (sha256 mismatch: $a vs $b)"
    FAIL=$((FAIL + 1)); return
  fi

  echo "[  PASS  ] $name ($(stat -c%s "$src") bytes)"
  PASS=$((PASS + 1))
}

# expect_rejected <name> <filename> <client args...>
expect_rejected() {
  local name="$1" file="$2"; shift 2
  if "$CLIENT" 127.0.0.1 "$PORT" "$SRC/$file" --quiet "$@" > "$WORK/client.log" 2>&1; then
    echo "[  FAIL  ] $name (transfer succeeded but should have been refused)"
    FAIL=$((FAIL + 1)); return
  fi
  echo "[  PASS  ] $name (refused: $(tail -1 "$WORK/client.log"))"
  PASS=$((PASS + 1))
}

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
: > "$SRC/empty.bin"                                  # 0 bytes
printf 'x' > "$SRC/one.bin"                           # 1 byte
head -c 100000 /dev/urandom > "$SRC/multi.bin"        # 84 chunks
head -c 5242880 /dev/urandom > "$SRC/large.bin"       # 5 MiB binary

# A file whose bytes include every possible byte value, so a transfer that
# mangled a particular byte (a NUL, a 0xFF, a newline) would show up.
python3 -c "
import sys
sys.stdout.buffer.write(bytes(range(256)) * 40)
" > "$SRC/allbytes.bin" 2>/dev/null || head -c 10240 /dev/urandom > "$SRC/allbytes.bin"

echo "=== SwiftLink integration tests (port $PORT) ==="
start_server || exit 1

run_case "empty file"                empty.bin
run_case "single byte"               one.bin
run_case "multi-chunk (100 KB)"      multi.bin
run_case "large binary (5 MiB)"      large.bin
run_case "every byte value"          allbytes.bin

run_case "multi-chunk, 5% loss"      multi.bin --simulate-loss=0.05
run_case "multi-chunk, 20% loss"     multi.bin --simulate-loss=0.20
run_case "large binary, 5% loss"     large.bin --simulate-loss=0.05

run_case "window=1 (stop-and-wait)"  multi.bin --window=1
run_case "window=128"                multi.bin --window=128

# Traversal is *contained*, not refused: the name is reduced to its final
# component, so this writes DST/swiftlink_pwned and cannot escape. The
# containment assertion below is the one that matters.
run_case_as "path traversal contained" multi.bin swiftlink_pwned \
  --name=../../../../tmp/swiftlink_pwned
expect_rejected "shell metacharacters refused" multi.bin '--name=evil; rm -rf /'

# The traversal attempt must not have written anywhere outside the output dir.
if [ -e /tmp/swiftlink_pwned ]; then
  echo "[  FAIL  ] traversal containment (file escaped to /tmp)"
  FAIL=$((FAIL + 1))
else
  echo "[  PASS  ] traversal containment (nothing written outside output dir)"
  PASS=$((PASS + 1))
fi

# Reverse-path loss: the FIN_ACK itself can be the packet that goes missing.
#
# This case exists because of a real defect. --simulate-loss drops only
# *outbound* datagrams at the client, so nothing above this line can ever lose
# an ACK, and the bug hid there for four milestones: the server erased a
# session the instant it completed, so a retransmitted FIN found nobody home
# and the sender reported failure for a file already sitting complete and
# byte-identical on disk. Reproduced at 9 failures in 20 runs with 30% reverse
# loss; the fix is the linger window in ServerConfig.
#
# The proxy impairs only the server->client direction, which is the half
# --simulate-loss cannot reach.
if [ -x "$PROXY" ]; then
  rev_ok=1
  rev_detail=""
  for i in 1 2 3 4 5 6; do
    pport=$(( PORT + 1000 + i ))
    "$PROXY" "$pport" "$PORT" 2000 20000 --seed=$(( 900 + i )) \
      --loss=40% --impair=to-client > "$WORK/proxy$i.log" 2>&1 &
    prx=$!
    for _ in $(seq 1 100); do
      grep -q READY "$WORK/proxy$i.log" 2>/dev/null && break
      sleep 0.05
    done

    rm -f "$DST/rev$i.bin"
    cp "$SRC/multi.bin" "$SRC/rev$i.bin"
    if ! timeout 120 "$CLIENT" 127.0.0.1 "$pport" "$SRC/rev$i.bin" --quiet \
         --rto-ms=100 > "$WORK/rev$i.log" 2>&1; then
      rev_ok=0
      rev_detail="run $i: $(tail -1 "$WORK/rev$i.log")"
    fi
    kill "$prx" 2>/dev/null; wait "$prx" 2>/dev/null

    a=$(sha256sum "$SRC/rev$i.bin" | cut -d' ' -f1)
    b=$(sha256sum "$DST/rev$i.bin" 2>/dev/null | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then
      rev_ok=0
      rev_detail="run $i: sha256 mismatch"
    fi
  done
  if [ "$rev_ok" = 1 ]; then
    echo "[  PASS  ] 6 runs at 40% reverse-path loss (FIN_ACK recovery)"
    PASS=$((PASS + 1))
  else
    echo "[  FAIL  ] 6 runs at 40% reverse-path loss ($rev_detail)"
    FAIL=$((FAIL + 1))
  fi
else
  echo "[  SKIP  ] reverse-path loss (delay_proxy not built)"
fi

# Concurrency: several clients against the one epoll server at once.
pids=""
for i in 1 2 3 4; do
  cp "$SRC/multi.bin" "$SRC/conc$i.bin"
  "$CLIENT" 127.0.0.1 "$PORT" "$SRC/conc$i.bin" --quiet --simulate-loss=0.02 \
    > "$WORK/conc$i.log" 2>&1 &
  pids="$pids $!"
done
conc_ok=1
for p in $pids; do wait "$p" || conc_ok=0; done
for i in 1 2 3 4; do
  a=$(sha256sum "$SRC/conc$i.bin" | cut -d' ' -f1)
  b=$(sha256sum "$DST/conc$i.bin" 2>/dev/null | cut -d' ' -f1)
  [ "$a" = "$b" ] || conc_ok=0
done
if [ "$conc_ok" = 1 ]; then
  echo "[  PASS  ] 4 concurrent transfers"
  PASS=$((PASS + 1))
else
  echo "[  FAIL  ] 4 concurrent transfers"
  FAIL=$((FAIL + 1))
fi

stop_server

echo
echo "$((PASS + FAIL)) cases, $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
