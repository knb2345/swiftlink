# Deferred work

Everything consciously left for a polish pass, with why. This is not a list of
things that are broken — the system works end to end and every test passes —
it is the list of things a second pass should address.

## Measurement gaps

- **M3 Condition B loss rates not measured.** Window 8 and 32 were measured at
  0% loss under 26.4 ms RTT; 1% and 5% were skipped for time. Recorded as "not
  measured" in `docs/benchmarks.md` rather than estimated.
- **M3 window sizes 4, 16 and 64 skipped**, at the user's request, to shorten
  the run. 1, 8 and 32 were measured.
- **Only one run at window 1 under delay**, because each takes ~4 minutes. The
  M2 table has three runs at the equivalent setting.
- **netem never used.** This kernel is built without it
  (`CONFIG_NET_SCH_NETEM is not set`). All delay figures come from
  `tools/delay_proxy.cpp`, which emulates delay only — no reordering,
  duplication, or corruption. Running the matrix under real netem on a normal
  kernel would be the honest cross-check, and would also let the reordering
  path be exercised for the first time.
- **`SO_RCVBUF` ceiling not explored.** Window 128 collapses because the
  receive queue overflows at `net.core.rmem_max = 212992`. Raising it needs
  root, so the relationship was reasoned from the numbers rather than measured
  across buffer sizes.

## Protocol and implementation

- **Fixed 300 ms RTO.** The benchmarks show it is badly mismatched in both
  directions: ~1700x the RTT on loopback, ~11x under 26 ms delay. Should be an
  adaptive estimate (Jacobson/Karels: smoothed RTT plus 4x mean deviation, with
  Karn's algorithm to avoid sampling retransmitted packets).
- **No congestion control at all.** The window is a fixed flow-control limit,
  not a congestion window. There is no slow start, no AIMD, no reaction to
  loss beyond retransmitting. Sending at window 128 into a buffer that holds
  ~100 is exactly the behaviour congestion control exists to prevent.
- **No fast retransmit.** Every loss costs a full RTO even when later ACKs
  prove the gap. Duplicate-ACK-triggered retransmission would recover most
  losses in roughly one RTT instead of 300 ms.
- **Window size is not negotiated.** The receiver defaults to a large window
  (1024) so a client with a bigger window still interoperates, but the correct
  fix is to agree a value during the START/START_ACK exchange.
- **No path MTU discovery.** The 1200-byte chunk is a conservative constant.
- **Sequence numbers cannot wrap.** 32 bits at 1200 bytes per chunk caps a
  transfer at ~5 TB; `send_file` refuses anything larger rather than wrapping.
  Proper wraparound handling with serial-number arithmetic (RFC 1982) is the
  real fix.
- **ACKs are individual, not cumulative or selective-ranged.** One ACK per
  packet is simple and correct but doubles the packet count. A SACK-style
  range encoding would cut the reverse-path traffic substantially.

## Robustness and security

- **No rate limiting or handshake cookies.** Only START may allocate a session,
  which stops the cheapest attack, but nothing stops a flood of STARTs from
  exhausting memory and file descriptors. A SYN-cookie-style stateless
  handshake would fix it properly.
- **Session ids are `mt19937_64` seeded from `random_device`, not a CSPRNG.**
  Fine for collision avoidance, not for unguessability by a determined
  attacker. Should be `getrandom(2)`.
- **Filename collisions are unhandled.** Two concurrent transfers advertising
  the same name write to the same path and corrupt each other. Needs either
  per-session temporary files renamed on completion, or outright rejection.
- **Partial files are left on disk** when a session is abandoned or fails
  integrity. Writing to a temporary name and renaming only after the SHA-256
  check passes would mean a file in the output directory is always complete
  and verified.
- **Filename sanitisation rejects all non-ASCII**, which is stricter than POSIX
  and would break international filenames. Deliberate for now; would need a
  proper UTF-8 validator to relax safely.
- **No IPv6.** `UdpSocket` is `AF_INET` only. `Endpoint` would become a
  `sockaddr_storage` wrapper.
- **No DNS.** The client takes dotted-quad addresses only.
- **`ReceiverSession` re-reads the whole file to hash it**, doubling disk reads
  on completion. Unavoidable given out-of-order arrival, but it could hash
  opportunistically over the contiguous prefix as the base advances and only
  re-read the rest.

## Testing

- **Sub-chunk and exact-chunk-multiple integration cases skipped** at the
  user's request. The unit tests cover the chunk-count arithmetic and M2 tested
  these sizes manually (1199/1200/1201/2400 all verified), but they are not in
  the automated suite.
- **No test injects packet corruption**, so the CRC32 rejection path is proven
  only by unit tests on the primitive, never end to end. A proxy that flips a
  bit would close that gap.
- **No test forces reordering**, since loopback does not reorder and the delay
  proxy preserves order. The out-of-order receive path is therefore exercised
  only by unit tests, not by an actual reordered transfer.
- **No fuzzing of `deserialize`.** It is the one function that parses hostile
  input, so it is the obvious candidate for a fuzz target.
- **ASan/UBSan/TSan never run.** A sanitiser build in CI would be cheap.
- **Concurrency tested at 4 sessions.** Nothing has probed hundreds.

## Build and tooling

- **`CMAKE_BUILD_TYPE` defaults to Debug**, where SHA-256 runs at 28 MB/s
  against 179 MB/s at `-O2`. Any benchmark that forgets `-DCMAKE_BUILD_TYPE=Release`
  is really measuring unoptimised crypto. Worth a warning at configure time.
- **No CI.** No install target. No `clang-format`/`clang-tidy` config.
