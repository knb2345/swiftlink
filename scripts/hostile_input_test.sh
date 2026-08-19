#!/usr/bin/env bash
#
# Hostile input: what a public UDP port actually receives.
#
# The server binds 0.0.0.0 and will accept a datagram from anyone. Everything
# below is what an unauthenticated stranger can send it. The bar is not that
# these are handled gracefully in the abstract -- it is that:
#
#   1. the server process survives all of it,
#   2. nothing is written outside the output directory,
#   3. a legitimate transfer running *at the same time* still completes and
#      still verifies byte-identical.
#
# (3) is the one that matters. A server that survives garbage by wedging, or by
# letting a stray datagram into somebody else's file, has not passed.
#
# The attacker is written in Python against the documented wire format rather
# than by calling the project's own serialiser. That is deliberate: a bug in
# serialize() would otherwise produce "malformed" packets that are malformed in
# exactly the way the decoder expects, and the test would prove nothing.
#
# Usage: hostile_input_test.sh [path-to-build-dir]

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-release}"
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

PORT=$(( 23000 + (RANDOM % 15000) ))
DST="$WORK/dst"
mkdir -p "$DST"
PASS=0
FAIL=0

note() { echo "[  PASS  ] $1"; PASS=$((PASS + 1)); }
bad()  { echo "[  FAIL  ] $1"; FAIL=$((FAIL + 1)); }

SRC="$WORK/payload.bin"
head -c 1048576 /dev/urandom > "$SRC"
SRC_SHA=$(sha256sum "$SRC" | cut -d' ' -f1)

"$SERVER" "$PORT" "$DST" --quiet --idle-ms=180000 > "$WORK/server.log" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 200); do
  grep -q READY "$WORK/server.log" 2>/dev/null && break
  sleep 0.05
done
kill -0 "$SRV_PID" 2>/dev/null || { echo "server failed to start" >&2; exit 1; }

echo "=== SwiftLink hostile input (port $PORT) ==="

# ---------------------------------------------------------------------------
# The attacker
# ---------------------------------------------------------------------------
cat > "$WORK/attack.py" <<'PY'
import os, random, socket, struct, sys, zlib

PORT = int(sys.argv[1])
MODE = sys.argv[2]
ADDR = ("127.0.0.1", PORT)

MAGIC, VERSION, HDR = 0x53574C4B, 1, 40
START, START_ACK, DATA, ACK, FIN, FIN_ACK, ERROR = range(1, 8)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
random.seed(20260817)
sent = 0


def header(ptype, session, seq=0, ack=0, offset=0, payload=b"",
           magic=MAGIC, version=VERSION, hdr_len=HDR, plen=None, flags=0):
    """Builds a packet exactly as the wire format documents it, so that any
    field can be made wrong independently of the others."""
    if plen is None:
        plen = len(payload)
    raw = struct.pack(
        ">IBBHQIIQHHI", magic, version, ptype, hdr_len, session, seq, ack,
        offset, plen, flags, 0) + payload
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    return raw[:36] + struct.pack(">I", crc) + raw[40:]


def shoot(data):
    global sent
    sock.sendto(data, ADDR)
    sent += 1


if MODE == "garbage":
    # Bytes that were never a SwiftLink packet at all.
    for _ in range(2000):
        shoot(os.urandom(random.randint(0, 2048)))
    shoot(b"")
    shoot(b"\x00" * 65507)

elif MODE == "truncated":
    # Every prefix of a valid header, so the decoder's length checks are
    # exercised at each boundary rather than only at zero.
    full = header(START, 0x1111, payload=b"t.bin")
    for n in range(0, HDR + 6):
        shoot(full[:n])

elif MODE == "fields":
    good = dict(ptype=START, session=0x2222, payload=b"f.bin")
    shoot(header(**{**good, "magic": 0xDEADBEEF}))       # not ours
    shoot(header(**{**good, "magic": 0}))
    shoot(header(**{**good, "version": 0}))              # older
    shoot(header(**{**good, "version": 255}))            # newer
    shoot(header(**{**good, "hdr_len": 0}))
    shoot(header(**{**good, "hdr_len": 39}))
    shoot(header(**{**good, "hdr_len": 65535}))
    shoot(header(**{**good, "plen": 65535}))             # claims more than sent
    shoot(header(**{**good, "plen": 0}))                 # claims less than sent
    shoot(header(**{**good, "flags": 0xFFFF}))
    for t in (0, 8, 9, 127, 200, 255):                   # not a packet type
        shoot(header(t, 0x2222, payload=b"x"))
    for t in (START_ACK, ACK, FIN_ACK, ERROR):           # our own replies
        shoot(header(t, 0x2222, payload=b"x"))
    # A packet that is perfect except for its checksum.
    ok = bytearray(header(**good))
    ok[36] ^= 0xFF
    shoot(bytes(ok))

elif MODE == "protocol":
    # Structurally valid, semantically hostile.
    shoot(header(DATA, 0x3333, seq=0, offset=0, payload=b"no start"))
    shoot(header(FIN, 0x3333, seq=1, offset=1 << 62, payload=b"\x00" * 32))
    shoot(header(START, 0x3334, offset=1 << 63, payload=b"huge.bin"))
    shoot(header(START, 0x3335, payload=b"A" * 65000))       # absurd name
    shoot(header(START, 0x3336, payload=b""))                # empty name
    shoot(header(START, 0x3337, payload=b"a\x00b.bin"))       # embedded NUL
    shoot(header(START, 0x3338, payload=b"a\nb\r\t.bin"))     # control bytes
    shoot(header(START, 0x3339, payload=b"////"))             # all separators
    shoot(header(START, 0x333A, payload=b".."))
    shoot(header(START, 0x333B, payload=b"-rf"))              # leading dash
    shoot(header(START, 0x333C, payload=b"../" * 200 + b"esc"))
    shoot(header(START, 0x333D, payload="wideé中.bin".encode()))
    # Open a session, then write to it under a different session id.
    shoot(header(START, 0x4444, offset=4096, payload=b"hijack.bin"))
    shoot(header(DATA, 0x4445, seq=0, offset=0, payload=b"Z" * 1200))
    # Sequence numbers far outside any window.
    shoot(header(DATA, 0x4444, seq=0xFFFFFFFF, offset=0, payload=b"Z" * 8))
    shoot(header(DATA, 0x4444, seq=1 << 30, offset=1 << 40, payload=b"Z" * 8))
    # A FIN whose digest is the right length but the wrong value.
    shoot(header(FIN, 0x4444, seq=1, offset=4096, payload=b"\xAA" * 32))

elif MODE == "flood":
    # Many distinct session ids from one address: the session table is keyed by
    # (address, port, session id), so this is the cheapest way for a stranger
    # to make the server allocate.
    for i in range(4000):
        shoot(header(START, 0x50000 + i, offset=1 << 20,
                     payload=("flood%d.bin" % i).encode()))

print(sent)
PY

# ---------------------------------------------------------------------------
# Fire everything, with a legitimate transfer running underneath it
# ---------------------------------------------------------------------------
"$CLIENT" 127.0.0.1 "$PORT" "$SRC" --quiet --name=legit.bin --rto-ms=300 \
  > "$WORK/legit.log" 2>&1 &
LEGIT=$!

total=0
for mode in garbage truncated fields protocol flood; do
  n=$(python3 "$WORK/attack.py" "$PORT" "$mode" 2>>"$WORK/attack.err")
  if [ -z "$n" ]; then
    bad "$mode (attacker itself failed: $(tail -1 "$WORK/attack.err"))"
    continue
  fi
  total=$(( total + n ))
  if kill -0 "$SRV_PID" 2>/dev/null; then
    note "$mode ($n datagrams, server alive)"
  else
    bad "$mode ($n datagrams, SERVER DIED)"
    break
  fi
done

legit_rc=0
wait "$LEGIT" || legit_rc=$?

echo
if [ "$legit_rc" -eq 0 ]; then
  got=$(sha256sum "$DST/legit.bin" 2>/dev/null | cut -d' ' -f1)
  if [ "$got" = "$SRC_SHA" ]; then
    note "concurrent legitimate transfer completed byte-identical"
  else
    bad "concurrent legitimate transfer CORRUPTED (sha mismatch)"
  fi
else
  bad "concurrent legitimate transfer failed ($(tail -1 "$WORK/legit.log"))"
fi

# Nothing may exist outside the output directory, and inside it only the files
# a legitimate client asked for.
escaped=0
for probe in /tmp/esc /tmp/hijack.bin /tmp/huge.bin "$WORK/esc" "$WORK/hijack.bin"; do
  [ -e "$probe" ] && escaped=1
done
if [ "$escaped" -eq 0 ]; then
  note "nothing written outside the output directory"
else
  bad "a file escaped the output directory"
fi

if kill -0 "$SRV_PID" 2>/dev/null; then
  note "server still serving after $total hostile datagrams"
else
  bad "server not running at end of test"
fi

# The START flood must have been refused rather than absorbed. Without a limit
# it produced one session and one file per datagram; see docs/todo.md.
# The count comes from the shutdown line, so it is read after the server exits.
check_refusals() {
  local refused
  refused=$(sed -n 's/.*, \([0-9]*\) refused/\1/p' "$WORK/server.log")
  if [ -n "$refused" ] && [ "$refused" -gt 0 ]; then
    note "START flood refused at the session limit ($refused refused)"
  else
    bad "START flood was not refused (no admission control fired)"
  fi
}

# No flood datagram may have produced a file under the name it asked for. Only
# a verified transfer publishes a name into the output directory.
stray=$(find "$DST" -maxdepth 1 -name 'flood*' | wc -l)
if [ "$stray" -eq 0 ]; then
  note "no flood datagram produced a named output file"
else
  bad "$stray files in the output directory came from the flood"
fi

echo
echo "--- output directory while the server is still running ---"
ls -A "$DST" | sed 's/^/    /' | head -6
echo "    ($(ls -A "$DST" | wc -l) entries, $(find "$DST" -maxdepth 1 -name '.swiftlink-*.partial' | wc -l) of them in-progress temporaries)"

kill -INT "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; SRV_PID=""

# Once the sessions are gone, so are their temporaries: an abandoned transfer
# leaves the output directory exactly as it found it.
check_refusals
leftover=$(find "$DST" -maxdepth 1 -name '.swiftlink-*.partial' | wc -l)
remaining=$(ls -A "$DST" | tr '\n' ' ')
if [ "$leftover" -eq 0 ]; then
  note "no partial files left after shutdown (directory holds: $remaining)"
else
  bad "$leftover partial files left on disk after shutdown"
fi

echo
echo "--- server log ---"
tail -8 "$WORK/server.log" | sed 's/^/    /'

echo
echo "$((PASS + FAIL)) cases, $PASS passed, $FAIL failed ($total hostile datagrams sent)"
[ "$FAIL" -eq 0 ]
