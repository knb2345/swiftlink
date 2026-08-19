# Deferred work

Everything consciously left for a polish pass, with why. This is not a list of
things that are broken — the system works end to end and every test passes —
it is the list of things a second pass should address.

Entries removed by the hardening pass are not listed here; what that pass
measured, fixed and left alone is in [reliability.md](reliability.md).

## Measurement gaps

- **M3 Condition B loss rates not measured.** Window 8 and 32 were measured at
  0% loss under 26.4 ms RTT; 1% and 5% were skipped for time. Recorded as "not
  measured" in `docs/benchmarks.md` rather than estimated.
- **M3 window sizes 4, 16 and 64 skipped**, at the user's request, to shorten
  the run. 1, 8 and 32 were measured.
- **Only one run at window 1 under delay**, because each takes ~4 minutes. The
  M2 table has three runs at the equivalent setting.
- **netem never used.** This kernel is built without it
  (`CONFIG_NET_SCH_NETEM is not set`) and carries no `sch_netem` module, so it
  is unavailable at any privilege level. `tools/delay_proxy.cpp` now
  reimplements netem's models in userspace — uniform and correlated loss,
  Gilbert-Elliott burst loss, reordering, duplication, corruption — and
  `scripts/impair_test.sh` drives the matrix through it. Running the same
  matrix under real netem on a normal kernel is still the honest cross-check,
  because the proxy is an extra process in the data path rather than a qdisc.
- **netem's correlated loss is not reproducible by its stated rate.** The proxy
  implements `get_crandom` faithfully, and faithfully inherits its defect: the
  AR(1) recursion's stationary distribution clusters near 0.5, so
  `--loss=5% --loss-corr=25%` delivers about 0.1%, not 5%. Use `--burst-loss`
  (Gilbert-Elliott), whose marginal rate and mean burst length are both
  actually achieved. The `--loss-corr` flag is kept for fidelity to netem and
  carries a warning in the tool's usage.
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

- **No handshake cookies.** `ServerConfig::max_sessions` (default 256) now caps
  simultaneous sessions and answers anything beyond it with `kServerBusy`, so a
  flood costs a bounded amount of state instead of an unbounded one —
  `scripts/hostile_input_test.sh` fires 4000 STARTs from one address and
  watches ~1900 of them get refused. That is a mitigation, not the fix: a
  stranger can still occupy all 256 slots until the idle sweep reclaims them,
  and legitimate clients are refused while they do. A SYN-cookie-style
  stateless handshake, where the server keeps no state until the client proves
  it can receive, is the real answer. Per-source rate limiting is the cheaper
  partial one.
- **Session ids are `mt19937_64` seeded from `random_device`, not a CSPRNG.**
  Fine for collision avoidance, not for unguessability by a determined
  attacker. Should be `getrandom(2)`.
- **Filename collisions resolve last-writer-wins.** Concurrent transfers no
  longer corrupt each other — each writes to `.swiftlink-<session>.partial` and
  renames over the final name only after its SHA-256 verifies — but two
  transfers claiming one name still end with one file, whichever finished
  last. Rejecting the second START, or disambiguating the name, is the
  remaining decision.
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

- **ASan/UBSan/TSan never run.** A sanitiser build in CI would be cheap, and is
  the obvious next thing given how much hostile input the server now eats.
- **`deserialize` has no coverage-guided fuzzer.**
  `scripts/hostile_input_test.sh` throws several thousand random and
  deliberately-malformed datagrams at it, which is dumb fuzzing: it proves the
  decoder survives, not that its branches were reached. libFuzzer or AFL++
  against `deserialize` directly, with a corpus, is the real version.
- **The impairment matrix is single-client.** `tools/delay_proxy` tracks one
  client address, so loss and reordering cannot be combined with concurrency.
  The two are measured separately (`scripts/impair_test.sh` and
  `scripts/concurrency_test.sh`); a many-to-one impairing proxy would let them
  be measured together, which is the configuration a real deployment has.
- **No transfer above 4 GiB has been run.** The chunk-count cap itself is now
  tested at its exact boundary (`transfer::chunk_count`, exercised in
  `tests/session_tests.cpp`), but that is arithmetic, not I/O: nothing has moved
  enough bytes for a 32-bit truncation elsewhere in the pipeline to surface.
- **No test covers a real network path.** Everything runs over loopback with an
  emulator in between. Two hosts on a real link would be the honest check.

## Build and tooling

- **No CI.** No install target. No `clang-format`/`clang-tidy` config.
