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

## 2026-08-04 — M2: no handshake; server takes the output path on the CLI
Milestone 2's scope is stop-and-wait reliability, not session setup. Rather
than invent a half-handshake that milestone 4 would then replace, the server is
told its output path as an argument and the transfer is DATA/ACK plus a final
FIN. START/START_ACK/FIN_ACK, session IDs and filename negotiation land in M4
as specified. Deviation from the strict file layout: none; deviation from a
"complete" transfer protocol: deliberate, and confined to this milestone.

## 2026-08-04 — pread/pwrite instead of read/write plus lseek
Positional I/O carries the offset as an argument instead of mutating the
descriptor's shared file offset. Chunks can therefore be written in any order
(which milestone 3 requires), duplicate chunks rewrite the same bytes at the
same place so a retransmission cannot corrupt the file, and there is no
per-descriptor cursor for milestone 5's concurrent sessions to race on.

## 2026-08-04 — 1200-byte chunks
1200 + 40 = 1240 bytes on the wire, under the 1500-byte Ethernet MTU even after
20 bytes of IPv4 and 8 of UDP header, with room for VPN or PPPoE encapsulation.
Avoiding IP fragmentation matters because a fragmented datagram is lost entirely
if any single fragment is lost, which would multiply the effective loss rate.
Loopback's MTU is 65536 so nothing fragments during local benchmarking anyway;
the number is chosen for the real-network case.

## 2026-08-04 — SO_RCVTIMEO for the retransmission timer
With one packet in flight, a receive timeout is the whole timer implementation:
recvfrom returns EAGAIN and the sender retransmits. The deadline is recomputed
on each stray packet rather than restarting the full RTO, so an unrelated
sender cannot postpone a retransmission indefinitely. Milestone 5 replaces this
with epoll, which is the only way to wait on several sessions at once.

## 2026-08-04 — The receiver ACKs duplicates instead of ignoring them
A duplicate DATA packet means the sender never saw our ACK. Discarding it
silently would deadlock the transfer: the sender times out again, resends
again, and the receiver stays quiet each time. Re-acknowledging is what breaks
the loop.

## 2026-08-04 — Simulated loss counts as "sent"
A dropped datagram still increments packets_sent, because the sender's state
machine believes it sent it -- that is exactly what makes the simulation
faithful. simulated_drops is reported separately so the two are never confused.

## 2026-08-04 — netem unavailable; delay emulated in userspace instead
The M2 benchmark plan specified `tc qdisc add dev lo root netem delay 25ms`.
This WSL2 kernel is built without it: `zcat /proc/config.gz | grep
NET_SCH_NETEM` reports `# CONFIG_NET_SCH_NETEM is not set`, and `modinfo
sch_netem` finds no module, so the command fails at any privilege level.
Substitute: `tools/delay_proxy.cpp`, a single-threaded UDP relay holding each
datagram in a release-time min-heap and forwarding it via ppoll(). Its RTT was
measured (26.4ms for a nominal 25ms), not assumed. It cannot reorder, duplicate
or corrupt packets, so it emulates delay only. Flagged in docs/benchmarks.md.
Note that `netem delay 25ms` on lo would have given ~50ms RTT, since loopback
traverses the qdisc in both directions; the proxy is configured by one-way
delay so the resulting RTT is stated rather than inferred.

## 2026-08-04 — Benchmarks verify every run, and record runs not averages
Each benchmark run compares the received file's SHA-256 against the source and
reports FAILED rather than a number if they differ. A throughput figure from a
transfer that did not reproduce the file is worse than no figure. All three
runs per condition are recorded individually so variance is visible.
