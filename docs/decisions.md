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

## 2026-08-04 — M3: retransmission timers in a heap with lazy deletion
The brief asked for a timer structure that avoids scanning all outstanding
packets each loop iteration. A per-packet deadline array costs O(W) per
iteration and therefore O(N*W) per transfer -- the timer scan gets more
expensive precisely as you raise W to go faster. Instead deadlines live in a
binary min-heap: next deadline is O(1), schedule/expire are O(log W).
A heap cannot delete from the middle, which ACKs require, so cancellation is
lazy: each window slot carries a generation counter, each heap entry records
the generation it was created with, and cancelling bumps the slot's generation.
Entries whose generation no longer matches are discarded when they surface.

## 2026-08-04 — Out-of-order data is written straight to disk, never buffered
Each DATA packet carries its own byte_offset, so pwrite places it correctly
whether or not its predecessors have arrived. The receiver window therefore
stores one bit per sequence number (has it been written?) rather than holding
payloads in memory. Memory used by a receiving session is independent of the
window size, and the reordering buffer that a textbook Selective Repeat
receiver needs does not exist here at all.

## 2026-08-04 — Receiver window defaults much larger than the sender's
The receiver window exists only to recognise duplicates; a sequence beyond
base + capacity must be dropped because marking it would alias onto a live ring
slot. Defaulting the receiver to 1024 against a sender default of 32 means a
client configured with a larger window still interoperates, at a cost of one
bit per slot. Negotiating this properly belongs with the M4 handshake.

## 2026-08-04 — ReceiverSession is a state machine with no socket access
handle_packet() takes a decoded packet and returns what to send back, doing no
I/O. M3 drives it from a blocking loop and M5 drives it from epoll with many
sessions at once, without the reliability logic changing at all.

## 2026-08-05 — M4: CRC32 covers the packet, SHA-256 covers the file
Two checks at two layers, answering different questions. CRC32 is per packet,
computed over header and payload with the checksum field zeroed, and catches a
datagram corrupted in transit. It is explicitly not a security control: CRC32
is linear, so anyone able to modify a packet can also repair its checksum.
SHA-256 over the whole file in the FIN packet is the end-to-end check, and it
catches everything the per-packet CRC structurally cannot -- a chunk written to
the wrong offset, a reassembly bug of ours, a duplicate that overwrote good
data. UDP's own checksum is not sufficient for either job: 16 bits, and
optional in IPv4.

## 2026-08-05 — The receiver hashes by reading the file back, not streaming
Chunks can arrive out of order and SHA-256 is inherently sequential, so the
digest cannot be computed as packets arrive. Reading the finished file back
costs one extra pass over the data but produces a strictly stronger guarantee:
it verifies the bytes that actually landed on disk rather than the bytes that
went past on the wire. This is also why File::open_write uses O_RDWR rather
than O_WRONLY -- found the hard way, when the first end-to-end M4 run failed
with EBADF inside the verification pass.

## 2026-08-05 — Filename sanitisation is an allowlist, and rejects rather than cleans
Denylisting traversal patterns is a game the attacker wins ("....//" survives
naive ".." removal). Instead the name is reduced to its final path component
*once*, then required to match a narrow character set; anything else is
refused. Refusing rather than rewriting matters because a silently "cleaned"
name means the file that lands is not the file that was requested. Rejecting
non-ASCII is stricter than POSIX requires and would need relaxing for
international filenames -- noted in docs/todo.md.

## 2026-08-05 — Session ids are random per transfer and checked on every packet
The receiver requires every DATA and FIN packet to carry the id agreed during
the handshake. Without it, a delayed datagram from an earlier transfer to the
same port could inject bytes into the current file. A random 64-bit id also
means an off-path attacker must guess it to interfere.

## 2026-08-05 — A retransmitted START is re-acknowledged, never re-opened
If our START_ACK is lost the client resends START. Re-running the open path
would apply O_TRUNC and discard everything received so far, so a duplicate
START with a matching session id is simply acknowledged again.

## 2026-08-05 — M5: sessions keyed on client address *and* session id
Address alone is insufficient: NAT puts many clients behind one address, and a
single client can run concurrent transfers from different source ports. Session
id alone is insufficient too -- the client chooses it, so two clients could
collide, and an off-path attacker who guessed one could write into another
transfer. Requiring both means an attacker must guess the id *and* forge the
source address.

## 2026-08-05 — Single-threaded epoll rather than a thread per session
A session is almost entirely I/O wait, and the work per packet is small and
bounded: decode, one pwrite, one sendto. A thread each would spend its memory
and scheduler pressure on sleeping and would force locking around shared state.
One loop means there is no shared mutable state to protect, because only one
thread touches it.

## 2026-08-05 — Time and signals are turned into file descriptors
timerfd drives the idle sweep and signalfd delivers SIGINT/SIGTERM, so the loop
has exactly one blocking point. The usual alternative -- a signal handler
setting a volatile flag -- is constrained by async-signal-safety and races with
the blocking call itself, since a signal arriving just before epoll_wait would
not interrupt it. Note sigprocmask must block the signals first: signalfd does
not suppress normal delivery, so without it the first SIGINT kills the process
before the loop ever reads it.

## 2026-08-05 — Only START may create a session
Any other packet type for an unknown key is ignored rather than answered, so a
stray or hostile datagram cannot make the server allocate state. This is the
cheap half of DoS resistance; the expensive half (rate limiting, handshake
cookies) is listed in docs/todo.md.

## 2026-08-05 — Idle sessions are swept on a timer, not on every packet
Sweeping on the timerfd keeps the per-packet path free of bookkeeping that is
not needed at packet rate. The cost is that a session can linger up to one
sweep interval past its deadline, which is why the observed timeout in testing
was 1833 ms against a 1500 ms limit with a 500 ms sweep.
