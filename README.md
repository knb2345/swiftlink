# SwiftLink

A reliable file-transfer system built on UDP, in C++20 for Linux, with no
external dependencies.

UDP was chosen deliberately: everything that makes a transfer reliable —
sequencing, acknowledgement, retransmission, flow control, integrity — has to
be implemented by hand rather than inherited from the kernel.

## What it does

- **Selective Repeat sliding window** with a configurable size, per-packet
  retransmission timers held in a min-heap with lazy generation-counter
  deletion, and out-of-order reception written straight to disk via `pwrite`.
- **Session protocol**: START/START_ACK handshake, random per-transfer session
  ids validated on every packet, FIN/FIN_ACK teardown, structured ERROR packets
  carrying status codes.
- **Integrity at two layers**: CRC-32 per packet, SHA-256 over the whole file
  verified end to end. Both implemented from scratch and tested against
  published vectors.
- **Concurrent epoll server**: one thread, non-blocking socket, sessions keyed
  on client address plus session id, idle cleanup on a `timerfd`, graceful
  shutdown through a `signalfd`.
- **Path-traversal-safe filenames**: allowlist sanitisation, containment
  verified by an integration test that actually attempts an escape.
- **Atomic publication**: every transfer writes to a private
  `.swiftlink-<session>.partial` and renames over its final name only after the
  SHA-256 check passes, so a name in the output directory is always a complete,
  verified file and two clients claiming one name cannot shred each other.
- **Survives a hostile port**: the server is bound to `0.0.0.0` and takes
  datagrams from anyone, so `scripts/hostile_input_test.sh` fires several
  thousand malformed, truncated, and abusive ones at it — built against the
  documented wire format rather than through our own serialiser — while a
  legitimate transfer runs underneath and must still verify.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`CMAKE_BUILD_TYPE` defaults to Debug, where the from-scratch SHA-256 runs about
6x slower and the receiver hashes inside the window the client is timing. Use
Release for anything you intend to measure; configuring Debug prints a warning
saying so.

```sh
./build/swiftlink_server 9000 ./received          # runs until SIGINT
./build/swiftlink_client 127.0.0.1 9000 ./file.bin
```

Useful client flags: `--window=N`, `--simulate-loss=P`, `--seed=N`,
`--rto-ms=N`, `--chunk=N`, `--name=NAME`, `--quiet`.

## Testing and measurement

```sh
ctest --test-dir build-release --output-on-failure   # unit + integration + hostile input
scripts/impair_test.sh --trials=3                    # correctness under loss/reorder/dup/corrupt
scripts/concurrency_test.sh                          # how many simultaneous sessions hold
scripts/hostile_input_test.sh build-release          # malformed and abusive datagrams
scripts/benchmark.sh --build=build-release --windows=1,8,32
```

`scripts/impair_test.sh` drives every transfer through
[`tools/delay_proxy`](tools/delay_proxy.cpp), which reimplements netem's
impairment models in userspace — this kernel is built without netem
(`CONFIG_NET_SCH_NETEM is not set`), so `tc` is unavailable at any privilege
level. Every row in its output carries the impairment counters the proxy
actually applied, so a result is evidence rather than an assertion.

Results, method, and machine details are in
[docs/benchmarks.md](docs/benchmarks.md) (throughput) and
[docs/reliability.md](docs/reliability.md) (correctness under impairment).

The throughput headline: on a 26.4 ms RTT path, throughput scales linearly with
window size to within 2% across a 32x range — window 32 is 31.8x faster than
stop-and-wait, because every window tested is far below the ~2460-packet
bandwidth-delay product.

The correctness headline is in docs/reliability.md, along with the two defects
the impairment harness found: a completed transfer being reported as a failure
when its FIN_ACK was lost, and a sender that could block in `recvfrom` forever
because `SO_RCVTIMEO` reads a zero timeout as "no timeout".

## Layout

| Path | Role |
|---|---|
| `include/swiftlink/protocol/`, `src/protocol/` | Wire format, big-endian (de)serialisation, CRC-32, status codes. No sockets, no I/O. |
| `include/swiftlink/crypto/`, `src/crypto/` | SHA-256 from scratch. |
| `include/swiftlink/net/`, `src/net/` | RAII UDP socket. Moves opaque bytes; knows nothing about the packet format. |
| `include/swiftlink/io/`, `src/io/` | RAII file descriptors, positional (`pread`/`pwrite`) access. |
| `include/swiftlink/transfer/`, `src/transfer/` | Reliability: sliding windows, sender, receiver session state machine, filename sanitisation. |
| `include/swiftlink/server/`, `src/server/` | epoll event loop and session table. |
| `tools/` | `delay_proxy`, a UDP relay that impairs the path — delay, loss, burst loss, reordering, duplication, corruption — because this kernel lacks netem. |
| `tests/`, `scripts/` | Unit tests, integration tests, impairment and concurrency harnesses, hostile-input harness, benchmark driver. |

Dependencies flow one way: `server` → `transfer` → {`protocol`, `net`, `io`,
`crypto`}. The protocol layer never touches a socket, which is what makes the
whole wire format unit-testable without opening a file descriptor.

## Documents

- [docs/decisions.md](docs/decisions.md) — one entry per non-obvious choice, with the reasoning.
- [docs/benchmarks.md](docs/benchmarks.md) — every measured throughput number, per run, with the method.
- [docs/reliability.md](docs/reliability.md) — correctness under loss, reordering, duplication and corruption; the concurrency ceiling; and the defects the harness found.
- [docs/todo.md](docs/todo.md) — what is deliberately deferred, and why.
