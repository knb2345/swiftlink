# SwiftLink

A reliable file-transfer system over UDP in C++ for Linux.

Implements sequencing, Selective Repeat sliding windows, CRC32 packet
validation, SHA-256 end-to-end integrity, and an epoll-driven non-blocking
server supporting concurrent transfer sessions.

Status: Milestone 1 complete — packet format, serialisation, and a one-shot
UDP client/server exchange. Sliding window, CRC32, retransmission, epoll, and
file chunking are later milestones and are deliberately not present yet.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/swiftlink_server 9000          # blocks on one datagram, prints it, exits
./build/swiftlink_client 127.0.0.1 9000 "hello"
```

## Layout

| Path | Role |
|------|------|
| `include/swiftlink/protocol/`, `src/protocol/` | Packet format and (de)serialisation. No sockets, no I/O. |
| `include/swiftlink/net/`, `src/net/` | RAII UDP socket. Moves opaque bytes; knows nothing about the packet format. |
| `src/client/`, `src/server/` | The only code that wires the two layers together. |
| `tests/` | Serialisation round trip, exact wire-byte layout, and malformed-input rejection. |
