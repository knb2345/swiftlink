# Design decisions

Format: one entry per non-obvious choice, with the reasoning.

## 2026-08-04 — UDP over TCP
Chose UDP deliberately so the reliability layer must be implemented
by hand rather than provided by the kernel.

## 2026-08-04 — Field-by-field serialisation, never memcpy of the struct
`PacketHeader` is a host-side convenience type; the wire format is defined in
bytes and offsets. memcpy would leak host byte order onto the wire, depend on
compiler/ABI layout, and (in reverse) require reading an attacker-controlled
buffer as a misaligned struct. Full reasoning at the top of
`src/protocol/serializer.cpp`.

## 2026-08-04 — Hand-written big-endian helpers instead of htonl/htons
Two header fields are 64-bit and there is no portable `htonll` (glibc's
`htobe64` is an extension). Shift-based helpers cover all widths uniformly and
are correct on any host endianness without `#ifdef`.

## 2026-08-04 — DecodeResult instead of exceptions
Decoding runs on untrusted network input, where malformed data is expected
rather than exceptional. `DecodeResult` returns either a complete packet or a
`DecodeError`, never a half-built packet. `std::expected` would be the exact
fit but is C++23; the project is C++20.

## 2026-08-04 — Protocol and net are separate libraries
`swiftlink_protocol` has no OS dependency at all, so the whole packet format is
unit-testable without opening a file descriptor. `swiftlink_net` moves opaque
byte buffers and knows nothing about SwiftLink. Only the executables link both.

## 2026-08-04 — Hand-rolled test harness
The no-external-dependencies rule rules out GoogleTest. `tests/protocol_tests.cpp`
implements the part that matters: run every case, report failures with file and
line, exit non-zero. Registered with CTest.

## 2026-08-04 — UdpSocket move operations are written out, not defaulted
A defaulted move would member-wise copy the `int fd_` and leave the source
still owning it, so both destructors would `close()` the same descriptor.
Defaulted moves are only safe for members that clear themselves when moved from.
