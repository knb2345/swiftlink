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

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`CMAKE_BUILD_TYPE` defaults to Debug, where the from-scratch SHA-256 runs about
6x slower. Use Release for anything you intend to measure.

```sh
./build/swiftlink_server 9000 ./received          # runs until SIGINT
./build/swiftlink_client 127.0.0.1 9000 ./file.bin
```

Useful client flags: `--window=N`, `--simulate-loss=P`, `--seed=N`,
`--rto-ms=N`, `--chunk=N`, `--name=NAME`, `--quiet`.

## Benchmarking

```sh
scripts/benchmark.sh --build=build --size=10485760 --runs=3 --windows=1,8,32
scripts/integration_test.sh build
```

Results, method, and machine details are in [docs/benchmarks.md](docs/benchmarks.md).
The headline: on a 26.4 ms RTT path, throughput scales linearly with window
size to within 2% across a 32x range — window 32 is 31.8x faster than
stop-and-wait, because every window tested is far below the ~2460-packet
bandwidth-delay product.

## Layout

| Path | Role |
|---|---|
| `include/swiftlink/protocol/`, `src/protocol/` | Wire format, big-endian (de)serialisation, CRC-32, status codes. No sockets, no I/O. |
| `include/swiftlink/crypto/`, `src/crypto/` | SHA-256 from scratch. |
| `include/swiftlink/net/`, `src/net/` | RAII UDP socket. Moves opaque bytes; knows nothing about the packet format. |
| `include/swiftlink/io/`, `src/io/` | RAII file descriptors, positional (`pread`/`pwrite`) access. |
| `include/swiftlink/transfer/`, `src/transfer/` | Reliability: sliding windows, sender, receiver session state machine, filename sanitisation. |
| `include/swiftlink/server/`, `src/server/` | epoll event loop and session table. |
| `tools/` | `delay_proxy`, a UDP delay relay used for benchmarking (this kernel lacks netem). |
| `tests/`, `scripts/` | Unit tests, integration tests, benchmark driver. |

Dependencies flow one way: `server` → `transfer` → {`protocol`, `net`, `io`,
`crypto`}. The protocol layer never touches a socket, which is what makes the
whole wire format unit-testable without opening a file descriptor.

## Documents

- [docs/decisions.md](docs/decisions.md) — one entry per non-obvious choice, with the reasoning.
- [docs/benchmarks.md](docs/benchmarks.md) — every measured number, per run, with the method.
- [docs/todo.md](docs/todo.md) — what is deliberately deferred, and why.
