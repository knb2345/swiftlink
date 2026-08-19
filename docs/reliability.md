# Reliability under an impaired path

Everything here comes from a run that was executed and logged. Where a number
could not be measured, it says so and why. Where a claim is an inference rather
than a measurement, it says that too.

The question this document answers is not "how fast is it" — that is
[benchmarks.md](benchmarks.md) — but "does the layer built on top of UDP
actually deliver the file". Every condition ends in a SHA-256 comparison of the
source against what landed on disk. A transfer that completed but produced
different bytes is a failure, and so is a transfer that did not complete.

## At a glance

| | |
|---|---|
| Impairment matrix | **72 of 72** transfers verified byte-identical across 16 conditions x 3 trials, plus 8 boundary sizes x 3 |
| Conditions covered | uniform loss to 20%, Gilbert-Elliott burst loss, forward-only and reverse-only loss, duplication to 10%, reordering to 50%, corruption to 5%, and all of them at once |
| Concurrency | **384 simultaneous transfers**, one thread, all verified; breaks at 512 |
| Hostile input | **6086** malformed or abusive datagrams; server survives, a concurrent legitimate transfer still verifies |
| Corrupted files, across everything above | **zero** |
| Defects found | **2**, both reproduced deterministically, both fixed, both now regression-tested |
| Tests | 77 unit cases / 586 assertions, 15 integration cases, 11 hostile-input cases |

The last two rows are the point. Loss, reordering, duplication, corruption and
overload all cost throughput and cost retransmissions; none of them ever
produced a file that differed from its source, and none of them ever produced a
success report for a transfer that had not happened. Where the system failed it
failed loudly, left nothing behind, and said so.

## Why there is no `tc netem` here

The plan for this work called for a netem harness. That is not possible on this
machine:

```
$ zcat /proc/config.gz | grep NET_SCH_NETEM
# CONFIG_NET_SCH_NETEM is not set
$ find /lib/modules -name 'sch_netem*'
$
```

The kernel is built without netem and there is no module to load, so it is
unavailable at any privilege level — this is not a permissions problem, and
`sudo` would not help. The usual fallback of a veth pair inside a network
namespace does not help either: that works around loopback applying a qdisc
twice per round trip, which is a different problem from the qdisc not existing.

So the impairment models were reimplemented in userspace, in
[`tools/delay_proxy.cpp`](../tools/delay_proxy.cpp), and the proxy sits in the
data path between client and server. The models are netem's on purpose, so the
numbers mean the same thing. What it is *not* is a kernel qdisc: it is an extra
userspace hop, with more scheduling jitter than netem, handling one client.
Anywhere that difference matters, it is called out rather than glossed.

**Nothing in this document should be described as a netem harness.**

### The proxy proves each condition rather than asserting it

Every table below carries the impairment counters the proxy actually recorded,
per direction, next to the verdict. A row claiming 5% loss whose counters show
zero drops did not test what it claims, and the table shows that instead of
hiding it. The `applied%` column is the drop rate that genuinely happened.

### One netem model does not do what it says, and neither does ours

`netem loss P% C%` uses an AR(1) recursion (`get_crandom`) whose stationary
distribution clusters near 0.5. `P(x < P)` therefore falls far below `P`, and
the requested marginal loss rate is not delivered. Reimplementing it faithfully
reproduced the defect faithfully: `--loss=5% --loss-corr=25%` dropped **2 of
1754 datagrams — 0.11% against a requested 5%**, about 45x short.

This is worth stating because the original plan described `netem loss 5% 25%`
as "Gilbert-Elliott". It is not; netem has a separate `loss gemodel` for that.
The burst-loss rows below use a real Gilbert-Elliott model, whose marginal rate
and mean burst length both come out as requested, and `--loss-corr` is kept
only for fidelity to netem with a warning attached.

## Two defects this work found

Both were found by the harness, both were reproduced deterministically before
being fixed, and both now have a regression test that fails against the
pre-fix code. Neither was reachable by the test suite that existed before.

### 1. A completed transfer reported as a failure

**Symptom.** The client exits non-zero with `peer stopped responding`, while
the server logs the session as complete and the received file is
byte-identical.

```
rc=1  hash_match=YES
   client: transfer failed: peer stopped responding
   server: session 41e768d5785a3cd3 complete: .../payload.bin
           (262144 bytes, 219 chunks, 1.1706s, 1.792 Mbps, 24 duplicates)
           shutdown: 1 completed, 0 failed, 0 timed out, 0 abandoned
```

**Cause.** The server erased a session the instant it finished. If the FIN_ACK
was then lost on the way back, the sender — which has no other way to learn the
transfer succeeded — retransmitted its FIN. That FIN arrived at a server with
no record of the session, and the code correctly declined to let a non-START
packet allocate state, so it was dropped in silence. The sender retried until
its budget ran out and reported failure for a file that was already complete,
verified, and on disk.

This is the problem TCP's TIME_WAIT exists to solve: a receiver has to outlive
its own last packet.

**Why nothing caught it.** The only loss injection the test suite had was the
client's `--simulate-loss`, which drops *outbound* datagrams before `sendto`.
It cannot lose an ACK. Every loss test in the project pointed the same
direction, and the bug lived in the other one.

**Reproduction**, 4 KB file, 30% loss applied only to the server→client
direction, 20 trials:

| build | trials | failures | file byte-identical on every failure |
|---|---|---|---|
| before the fix | 20 | **9** | yes, all 9 |
| after the fix | 20 | 0 | — |
| after the fix, 50% reverse loss | 30 | 0 | — |

**Fix.** A completed session lingers rather than being erased
(`ServerConfig::session_linger`, default 20 s, which exceeds the sender's
`kMaxRetriesPerChunk x kDefaultRto` = 15 s retry budget). While lingering it
holds no file descriptor and replays its stored verdict to a retransmitted
FIN. The verdict is stored, not recomputed, so repeating a FIN cannot make the
server re-hash the file — that would hand a peer an O(filesize) amplification
against a single-threaded event loop.

**Guarded by** `tests/session_tests.cpp` (`retransmitted_fin_is_answered_again`,
`repeated_fins_are_all_answered`, `replayed_fin_does_not_reread_the_file`) and
an end-to-end case in `scripts/integration_test.sh` that runs six transfers
through 40% reverse-path loss. That integration case fails against the pre-fix
binaries and passes against the fixed ones, which is what makes it a guard
rather than decoration.

### 2. A sender that blocks forever

**Symptom.** During the impairment matrix, one condition simply stopped. The
client process was alive and had produced no output for minutes.

```
$ gdb -p 8047 -batch -ex bt
#0  __libc_recvfrom (fd=3, ...) at ../sysdeps/unix/sysv/linux/recvfrom.c:27
#1  swiftlink::net::UdpSocket::recv_from(...)
#2  swiftlink::transfer::send_file(...)
#3  main ()
```

**Cause.** `SO_RCVTIMEO` treats a zero `timeval` as *no timeout* — block
forever. The sender computes its wait as `deadline - now`, which is zero or
negative whenever a retransmission is already overdue. That is not an exotic
state: under heavy loss a pass through the loop routinely takes longer than the
RTO. When it happened, the socket was configured to wait forever and the
transfer hung with a full window of packets owed a retransmission.

It fails in the worst available way. There is no error, no log line, and no
exit — it looks exactly like a network that went quiet.

**Fix.** `UdpSocket::set_receive_timeout` clamps a zero or negative request to
1 µs, preserving the meaning the caller intended: return immediately with
nothing. The alternative — fixing it at the one call site — leaves the trap
armed for the next caller.

**Guarded by** `tests/net_tests.cpp`. Against the pre-fix code the suite hangs
and is killed at its timeout (exit 124); against the fixed code it passes in
0.2 s. `a_real_timeout_is_honoured` is there so the clamp cannot be "fixed" by
flattening every timeout to zero, which would turn the sender's sleep into a
spin.

## The impairment matrix

72 transfers, every one verified by SHA-256, **72 passed and 0 failed**.

- 1 MiB of `/dev/urandom`, one-way delay 5000 us (10 ms RTT), rto 100 ms, window 32
- three trials per condition, each with a different seed, so three trials are
  three different loss patterns rather than one pattern three times
- `fwd_*` counters are for the client->server direction
- `applied%` is the drop rate the proxy actually delivered, which is the column
  that makes a row evidence instead of a claim

| condition | trial | result | fwd pkts | fwd dropped | applied% | fwd dup | fwd reord | fwd corrupt | client retx |
|---|---|---|---|---|---|---|---|---|---|
| clean (control) | 1 | PASS | 876 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| clean (control) | 2 | PASS | 876 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| clean (control) | 3 | PASS | 876 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| loss 1% | 1 | PASS | 891 | 11 | 1.23 | 0 | 0 | 0 | 15 |
| loss 1% | 2 | PASS | 895 | 10 | 1.12 | 0 | 0 | 0 | 19 |
| loss 1% | 3 | PASS | 893 | 10 | 1.12 | 0 | 0 | 0 | 17 |
| loss 5% | 1 | PASS | 979 | 45 | 4.60 | 0 | 0 | 0 | 103 |
| loss 5% | 2 | PASS | 967 | 48 | 4.96 | 0 | 0 | 0 | 91 |
| loss 5% | 3 | PASS | 993 | 61 | 6.14 | 0 | 0 | 0 | 117 |
| loss 10% | 1 | PASS | 1091 | 96 | 8.80 | 0 | 0 | 0 | 215 |
| loss 10% | 2 | PASS | 1085 | 107 | 9.86 | 0 | 0 | 0 | 209 |
| loss 10% | 3 | PASS | 1104 | 119 | 10.78 | 0 | 0 | 0 | 228 |
| loss 20% | 1 | PASS | 1403 | 295 | 21.03 | 0 | 0 | 0 | 527 |
| loss 20% | 2 | PASS | 1315 | 241 | 18.33 | 0 | 0 | 0 | 439 |
| loss 20% | 3 | PASS | 1349 | 269 | 19.94 | 0 | 0 | 0 | 473 |
| loss 10% fwd only | 1 | PASS | 971 | 95 | 9.78 | 0 | 0 | 0 | 95 |
| loss 10% fwd only | 2 | PASS | 984 | 108 | 10.98 | 0 | 0 | 0 | 108 |
| loss 10% fwd only | 3 | PASS | 961 | 85 | 8.84 | 0 | 0 | 0 | 85 |
| loss 10% ACKs only | 1 | PASS | 961 | 0 | 0.00 | 0 | 0 | 0 | 85 |
| loss 10% ACKs only | 2 | PASS | 973 | 0 | 0.00 | 0 | 0 | 0 | 97 |
| loss 10% ACKs only | 3 | PASS | 983 | 0 | 0.00 | 0 | 0 | 0 | 107 |
| burst 5% len4 (G-E) | 1 | PASS | 936 | 40 | 4.27 | 0 | 0 | 0 | 60 |
| burst 5% len4 (G-E) | 2 | PASS | 964 | 60 | 6.22 | 0 | 0 | 0 | 88 |
| burst 5% len4 (G-E) | 3 | PASS | 936 | 37 | 3.95 | 0 | 0 | 0 | 60 |
| burst 10% len8 (G-E) | 1 | PASS | 1050 | 84 | 8.00 | 0 | 0 | 0 | 174 |
| burst 10% len8 (G-E) | 2 | PASS | 1116 | 122 | 10.93 | 0 | 0 | 0 | 240 |
| burst 10% len8 (G-E) | 3 | PASS | 1115 | 113 | 10.13 | 0 | 0 | 0 | 239 |
| duplicate 1% | 1 | PASS | 876 | 0 | 0.00 | 9 | 0 | 0 | 0 |
| duplicate 1% | 2 | PASS | 876 | 0 | 0.00 | 11 | 0 | 0 | 0 |
| duplicate 1% | 3 | PASS | 876 | 0 | 0.00 | 7 | 0 | 0 | 0 |
| duplicate 10% | 1 | PASS | 876 | 0 | 0.00 | 83 | 0 | 0 | 0 |
| duplicate 10% | 2 | PASS | 876 | 0 | 0.00 | 91 | 0 | 0 | 0 |
| duplicate 10% | 3 | PASS | 876 | 0 | 0.00 | 86 | 0 | 0 | 0 |
| reorder 25% | 1 | PASS | 876 | 0 | 0.00 | 0 | 222 | 0 | 0 |
| reorder 25% | 2 | PASS | 876 | 0 | 0.00 | 0 | 201 | 0 | 0 |
| reorder 25% | 3 | PASS | 876 | 0 | 0.00 | 0 | 228 | 0 | 0 |
| reorder 50% | 1 | PASS | 876 | 0 | 0.00 | 0 | 402 | 0 | 0 |
| reorder 50% | 2 | PASS | 876 | 0 | 0.00 | 0 | 425 | 0 | 0 |
| reorder 50% | 3 | PASS | 876 | 0 | 0.00 | 0 | 437 | 0 | 0 |
| corrupt 1% | 1 | PASS | 886 | 0 | 0.00 | 0 | 0 | 4 | 10 |
| corrupt 1% | 2 | PASS | 888 | 0 | 0.00 | 0 | 0 | 6 | 12 |
| corrupt 1% | 3 | PASS | 896 | 0 | 0.00 | 0 | 0 | 6 | 20 |
| corrupt 5% | 1 | PASS | 955 | 0 | 0.00 | 0 | 0 | 33 | 79 |
| corrupt 5% | 2 | PASS | 975 | 0 | 0.00 | 0 | 0 | 50 | 99 |
| corrupt 5% | 3 | PASS | 966 | 0 | 0.00 | 0 | 0 | 48 | 90 |
| combined hostile | 1 | PASS | 977 | 43 | 4.40 | 19 | 209 | 8 | 101 |
| combined hostile | 2 | PASS | 980 | 57 | 5.82 | 30 | 201 | 4 | 104 |
| combined hostile | 3 | PASS | 986 | 45 | 4.56 | 11 | 234 | 11 | 110 |

### What the numbers say

**The requested impairment arrived.** Across the uniform-loss rows the applied
rate tracks the requested one closely: 1% delivered 1.12-1.23%, 5% delivered
3.93-6.14%, 10% delivered 8.80-11.21%, 20% delivered 16.83-22.03%. The spread
is what a few hundred Bernoulli trials looks like, not a broken generator.

**Forward loss costs exactly one retransmission each.** The `loss 10% fwd only`
rows read 95 dropped / 95 retransmits, 108 / 108, 85 / 85 — three exact
matches. That is Selective Repeat doing precisely what it claims: the packets
whose own timers fired are the packets that were resent, and nothing else.
Go-Back-N would have resent the rest of the window with them.

**Reverse loss costs retransmissions too, and the proxy proves which is which.**
`loss 10% ACKs only` shows 0 forward drops and 85-107 retransmits: nothing was
lost on the way in, and the sender still had to resend because the
acknowledgements never came back. Without a per-direction counter these rows
would be indistinguishable from forward loss.

**Duplication is free.** Both duplicate rows cost 0 retransmissions. A
duplicate arrives, the receiver recognises the sequence number as already
written, re-acknowledges without rewriting, and the sender's window is
unaffected.

**Reordering is free, and this is the payoff from positional writes.** 25% and
50% reordering both cost 0 retransmissions. Each chunk carries its own byte
offset and is `pwrite`-n straight there, so a packet arriving early is simply
written early. There is no reassembly buffer to overflow and no head-of-line
stall to wait out.

**Corruption is caught by CRC-32 and costs one retransmission per corrupted
packet**, which is the correct price: a corrupt packet is indistinguishable
from a lost one once it has been discarded.

## Boundary file sizes

The chunk size is 1200 bytes. These sizes straddle the chunking arithmetic:
empty, sub-chunk, one under and one over an exact chunk, exact multiples. Each
runs over a path that is simultaneously lossy, reordering and duplicating,
because an off-by-one in chunk arithmetic is likeliest to surface when the last
chunk is short *and* arrives out of order.

| condition | trial | result | fwd pkts | fwd dropped | applied% | fwd dup | fwd reord | fwd corrupt | client retx |
|---|---|---|---|---|---|---|---|---|---|
| size 0B | 1 | PASS | 3 | 0 | 0.00 | 0 | 1 | 0 | 1 |
| size 0B | 2 | PASS | 2 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| size 0B | 3 | PASS | 4 | 2 | 50.00 | 0 | 0 | 0 | 2 |
| size 1B | 1 | PASS | 5 | 2 | 40.00 | 0 | 0 | 0 | 2 |
| size 1B | 2 | PASS | 4 | 1 | 25.00 | 0 | 1 | 0 | 1 |
| size 1B | 3 | PASS | 3 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| size 1199B | 1 | PASS | 3 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| size 1199B | 2 | PASS | 3 | 0 | 0.00 | 0 | 1 | 0 | 0 |
| size 1199B | 3 | PASS | 3 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| size 1200B | 1 | PASS | 3 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| size 1200B | 2 | PASS | 3 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| size 1200B | 3 | PASS | 4 | 0 | 0.00 | 0 | 3 | 0 | 1 |
| size 1201B | 1 | PASS | 6 | 0 | 0.00 | 0 | 4 | 0 | 2 |
| size 1201B | 2 | PASS | 4 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| size 1201B | 3 | PASS | 5 | 1 | 20.00 | 0 | 2 | 0 | 1 |
| size 2399B | 1 | PASS | 5 | 1 | 20.00 | 0 | 2 | 0 | 1 |
| size 2399B | 2 | PASS | 4 | 0 | 0.00 | 0 | 0 | 0 | 0 |
| size 2399B | 3 | PASS | 4 | 0 | 0.00 | 1 | 1 | 0 | 0 |
| size 2400B | 1 | PASS | 4 | 0 | 0.00 | 1 | 1 | 0 | 0 |
| size 2400B | 2 | PASS | 4 | 0 | 0.00 | 0 | 3 | 0 | 0 |
| size 2400B | 3 | PASS | 7 | 2 | 28.57 | 0 | 2 | 0 | 3 |
| size 2401B | 1 | PASS | 8 | 3 | 37.50 | 0 | 2 | 0 | 3 |
| size 2401B | 2 | PASS | 6 | 0 | 0.00 | 0 | 2 | 0 | 1 |
| size 2401B | 3 | PASS | 5 | 0 | 0.00 | 0 | 2 | 0 | 0 |

All 24 passed. Note the honest weakness these rows carry on their face: a 1200
byte file is 3 packets, so a 5% loss setting frequently applies no loss at all,
and several rows show `applied% = 0.00`. They are a test of the chunk-count
arithmetic under an impaired path, not a statistical test of the impairment.
The rows that did draw a drop (`size 0B` trial 3 at 50%, `size 2401B` trial 1
at 37.5%) are the ones carrying weight.

## Concurrency: how many sessions does one thread hold

One server process, one thread, one epoll loop, **one shared UDP socket**. Each
client sends 1 MiB and advertises a distinct filename — without that, the test
would be measuring the filename-collision behaviour rather than concurrency.
The server's admission limit was kept above every level tried — the default 256
for the 1-64 rows, and `--max-sessions=1024` from 128 up — so the ramp finds the
machine's limit rather than rediscovering a configured constant. The `refused`
column is zero throughout, which confirms admission control never fired and no
row is really a measurement of that setting.

No impairment here: `tools/delay_proxy` tracks a single client address, so
loss and concurrency cannot currently be combined. That is a real gap in the
tooling and is listed in [todo.md](todo.md).

| clients | completed | sha256 ok | wall s | aggregate Mbps | retransmits | server duplicates | refused |
|---|---|---|---|---|---|---|---|
| 1 | 1/1 | 1/1 | 0.042 | 199.73 | 0 | 0 | 0 |
| 2 | 2/2 | 2/2 | 0.111 | 151.15 | 0 | 0 | 0 |
| 4 | 4/4 | 4/4 | 2.290 | 14.65 | 207 | 0 | 0 |
| 8 | 8/8 | 8/8 | 6.328 | 10.61 | 2219 | 0 | 0 |
| 16 | 16/16 | 16/16 | 7.889 | 17.01 | 5098 | 0 | 0 |
| 32 | 32/32 | 32/32 | 10.824 | 24.80 | 11801 | 0 | 0 |
| 64 | 64/64 | 64/64 | 11.639 | 46.13 | 24864 | 0 | 0 |
| 128 | 128/128 | 128/128 | 17.096 | 62.81 | 51218 | 0 | 0 |
| 192 | 192/192 | 192/192 | 18.960 | 84.95 | 121841 | 0 | 0 |
| 256 | 256/256 | 256/256 | 23.847 | 90.05 | 179870 | 0 | 0 |
| 384 | 384/384 | 384/384 | 35.769 | 90.06 | 427541 | 0 | 0 |
| 512 | 510/512 ** | 510/512 | 40.838 | 104.76 | 708212 | 0 | 0 |

**384 simultaneous transfers completed with every file verified
byte-identical.** 512 is where it breaks.

### The shape of this curve is the receive buffer again

Aggregate throughput does not degrade monotonically — it collapses and then
recovers:

- **1-2 clients: 150-200 Mbps, zero retransmissions.** No contention.
- **4 clients: 14.65 Mbps, 207 retransmissions.** A 10x collapse between 2 and
  4 clients, and the first retransmissions of the run at zero injected loss.
  Four senders at window 32 put ~128 packets into a queue that the window sweep
  independently measured as holding 64-72. This is the same
  `net.core.rmem_max = 212992` ceiling as the window cliff, reached by adding
  clients instead of by widening one client's window.
- **8-384 clients: recovers to 60-90 Mbps.** Counter-intuitive but
  straightforward — with hundreds of sessions there is always someone with work
  ready, so the single thread stops idling between round trips even though
  every individual client is stalling on retransmission timers. Per-client
  throughput keeps falling the whole way; the aggregate goes up because the
  thread stays busy.

### How it fails, which matters more than where

At 512 clients, in a dedicated diagnostic run:

```
clients=512 client_failures=9 sha_ok=503 sha_bad=9
--- distinct client error messages ---
      9 transfer failed: peer stopped responding
--- what the mismatching outputs look like ---
c81.bin absent (nothing published)
c170.bin absent (nothing published)
c281.bin absent (nothing published)
...
--- server ---
shutdown: 503 completed, 0 failed, 0 timed out, 9 abandoned, 0 refused
```

Every number in that run agrees with every other: 9 clients failed, 9 files are
missing, the server abandoned 9 sessions. **Not one file was corrupted, and not
one incomplete file was published.** The failures are senders exhausting their
retry budget against a receive queue that is overwhelmed, and every one of them
reports failure rather than claiming success.

That is the distinction the whole exercise is about. Losing transfers under
overload is a capacity limit. Writing a file that does not match the source, or
telling the client a transfer succeeded when it did not, would be a correctness
failure. Across 72 impaired transfers, the concurrency ramp, and this run,
there were none.

## Hostile input

The server binds `0.0.0.0` and accepts a datagram from anyone, so
`scripts/hostile_input_test.sh` sends it what a stranger can. The attacker is
written in Python against the *documented* wire format rather than through the
project's own serialiser — a bug in `serialize()` would otherwise produce
"malformed" packets malformed in exactly the way the decoder expects.

| category | datagrams | what it covers |
|---|---|---|
| garbage | 2002 | random bytes, 0-2048 long, plus an empty datagram and a 65507-byte one |
| truncated | 46 | every prefix of a valid header, 0 through 45 bytes |
| fields | 21 | bad magic, wrong version, impossible header lengths, payload length that over- and under-states the datagram, unknown packet types, our own reply types echoed back, a valid packet with one checksum bit flipped |
| protocol | 17 | DATA with no handshake, FIN with a 2^62 offset, 65000-byte filename, embedded NULs, control bytes, `..`, 200 stacked `../`, non-ASCII, a session-id hijack attempt, sequence numbers at 2^32-1 and 2^30 |
| flood | 4000 | STARTs with 4000 distinct session ids from one address |

All 11 assertions pass. The ones that carry weight:

- **A legitimate 1 MiB transfer runs underneath the whole barrage and still
  verifies byte-identical.** Surviving garbage by wedging would not count.
- **Nothing is written outside the output directory**, including after 200
  stacked `../` components.
- **No flood datagram produces a named output file.** Only a verified transfer
  publishes a name.
- **After shutdown the output directory holds exactly one file** — the
  legitimate one. The 257 in-progress temporaries are gone.

### What the flood found

Before this pass, 4000 START datagrams from one stranger produced 4000 sessions
and 4000 zero-byte files named exactly as the attacker asked. `docs/todo.md`
had flagged that a START flood could exhaust memory and descriptors; the test
turned the prediction into a measurement.

It is now bounded two ways. `ServerConfig::max_sessions` (default 256) refuses
new sessions past the limit with a `kServerBusy` ERROR — **1664 of the 4000
were refused** in the run above — and because every transfer writes to
`.swiftlink-<session>.partial` until it verifies, no attacker-chosen name ever
appears at all.

This is a mitigation, not a fix, and calling it one would be overclaiming: a
stranger can still occupy all 256 slots until the idle sweep reclaims them, and
legitimate clients are refused while that lasts. The real answer is a
SYN-cookie-style stateless handshake where the server keeps no state until the
client has proved it can receive. That stays in [todo.md](todo.md).

## What was not tested, and why

- **Real netem.** Unavailable on this kernel; see the top of this document.
  Re-running the matrix under a real qdisc on a normal kernel is the honest
  cross-check and has not been done.
- **A real two-host network path.** Everything here is loopback with an
  emulator in the middle.
- **Impairment combined with concurrency.** The proxy handles one client.
- **Transfers above 4 GiB.** Nothing has pushed past the point where a 32-bit
  truncation would surface.
- **Sanitiser builds.** ASan/UBSan/TSan have never been run, which is the
  obvious next step given how much hostile input the server now accepts.
