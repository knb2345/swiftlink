# Benchmarks

Every number in this file comes from a transfer that was actually executed and
verified byte-identical (SHA-256 of source vs received file) on the machine
described below. Individual runs are recorded, never an average standing in for
runs that were not performed. Where a measurement could not be taken, it says
"not measured" and why.

## Test machine

| Property | Value |
|---|---|
| CPU | 12th Gen Intel Core i5-12500H (16 logical cores) |
| RAM | 7 GiB available to the VM |
| OS | Ubuntu 24.04.4 LTS under **WSL2** |
| Kernel | 5.15.167.4-microsoft-standard-WSL2 |
| Compiler | g++ 13.3.0, `-std=c++20`, CMake 3.28.3, default `Debug` build type |
| Loopback MTU | 65536 bytes (`lo`, qdisc `noqueue`) |
| Loopback RTT (idle) | `ping -c 5 127.0.0.1` → min/avg/max/mdev = **0.038/0.091/0.219/0.065 ms** |
| UDP socket buffers | `net.core.rmem_default = rmem_max = wmem_default = 212992` |

Test file: 10 MiB (10,485,760 bytes) of `/dev/urandom`,
SHA-256 `316c5a30bae3532721f2b768e7a35bcc0d401e8ad52929f4dcf8c4d3f366f004`.
At a 1200-byte chunk size that is **8739 DATA packets + 1 FIN = 8740 packets**
minimum.

Throughput is decimal megabits per second (`bytes * 8 / elapsed / 1e6`),
measured client-side, timed from the first chunk read to the FIN being
acknowledged.

---

## Milestone 2 — stop-and-wait baseline

One DATA packet in flight, one ACK per packet, fixed 300 ms RTO, retransmission
on timeout. `--simulate-loss` drops a fraction of *outbound* datagrams at the
client before `sendto`, modelling forward-path loss.

### Condition A: loopback, no artificial delay

| condition | run | elapsed s | throughput Mbps | packets sent | retransmissions |
|---|---|---|---|---|---|
| 0% loss | 1 | 1.6989 | 49.377 | 8740 | 0 |
| 0% loss | 2 | 1.5461 | 54.256 | 8740 | 0 |
| 0% loss | 3 | 1.4552 | 57.645 | 8740 | 0 |
| 1% loss | 1 | 28.2935 | 2.965 | 8825 | 85 |
| 1% loss | 2 | 30.3644 | 2.763 | 8831 | 91 |
| 1% loss | 3 | 29.3589 | 2.857 | 8828 | 88 |
| 5% loss | 1 | 146.0351 | 0.574 | 9208 | 468 |
| 5% loss | 2 | 136.9026 | 0.613 | 9177 | 437 |
| 5% loss | 3 | 138.6377 | 0.605 | 9183 | 443 |

All nine runs verified byte-identical.

Notes on Condition A:

- **0% loss is bounded by per-packet cost, not bandwidth.** 1.5461 s / 8740
  packets = **177 µs per round trip**, against an idle loopback ping RTT of
  91 µs. The extra ~86 µs is the per-packet work: two `sendto`/`recvfrom` pairs,
  serialisation, a `pwrite`, and two process wakeups.
- **A separate calibration run before the matrix recorded 1.1954 s / 70.171 Mbps**
  under identical settings. The three recorded runs were executed back-to-back,
  so the ~15% spread between 1.46 s and 1.70 s reflects scheduler and cache
  variance on a machine that is not otherwise quiesced. Reported as observed.
- **Retransmission counts track the injected loss rate closely**, confirming the
  simulator: 1% of 8740 ≈ 87 (observed 85, 91, 88); 5% of 8740 ≈ 437 (observed
  468, 437, 443).
- **Time cost of loss is dominated by the fixed RTO.** Each lost packet costs a
  full 300 ms stall. 437 × 300 ms = 131 s, which is essentially all of the
  136.9 s in run 2 — the actual data movement is the 1.5 s baseline.
- **One genuine, non-simulated loss was observed.** In 5% run 1 the client
  reported `retransmissions=468, simulated_drops=467` while the server reported
  `packets_received=8741, duplicates=1`. One DATA packet was really sent but its
  ACK was lost or delayed past the 300 ms RTO; the client retransmitted, the
  server recognised the duplicate and re-acknowledged it rather than rewriting.
  That is the duplicate-handling path firing for real rather than in a test.

### Condition B: 25 ms artificial RTT

**Deviation from spec, flagged.** The plan specified
`sudo tc qdisc add dev lo root netem delay 25ms`. That is not possible on this
machine, and not because of permissions:

```
$ zcat /proc/config.gz | grep NET_SCH_NETEM
# CONFIG_NET_SCH_NETEM is not set
$ modinfo sch_netem
modinfo: ERROR: Module sch_netem not found.
```

The stock WSL2 kernel is compiled without netem — not built in, not available
as a module. `tc qdisc add ... netem` would fail with "Unknown qdisc" at any
privilege level. Rebuilding the WSL2 kernel was judged out of scope for a
milestone benchmark.

**Substitute method:** `tools/delay_proxy.cpp`, a single-threaded UDP relay that
holds each datagram in a release-time min-heap and forwards it after a fixed
one-way delay, sleeping in `ppoll()` until the earliest datagram is due. The
client sends to the proxy instead of the server; both directions are delayed, so
RTT = 2 × one-way delay. Configured at **12.5 ms one way**.

**Measured, not assumed:** a 12,000-byte file (10 chunks + FIN = exactly 11
round trips) took 0.2903 s through the proxy → **26.4 ms effective RTT**
(25 ms configured + ~1.4 ms for the userspace hop and scheduling).

How this differs from netem, stated plainly:

- It adds a userspace process to the path, costing the ~1.4 ms measured above.
- It cannot reorder, duplicate, or corrupt packets; netem can. Only delay is
  emulated.
- `netem delay 25ms` on `lo` would have produced roughly a **50 ms** RTT, not
  25 ms, because loopback traffic traverses the qdisc in both directions. This
  tool is configured by one-way delay so the resulting RTT is stated explicitly.
  Condition B numbers are therefore **not** directly comparable to a hypothetical
  netem run at the same nominal setting.

| condition | run | elapsed s | throughput Mbps | packets sent | retransmissions |
|---|---|---|---|---|---|
| 0% loss, 26.4 ms RTT | 1 | 230.3377 | 0.364 | 8740 | 0 |
| 0% loss, 26.4 ms RTT | 2 | 230.3295 | 0.364 | 8741 | 1 |
| 0% loss, 26.4 ms RTT | 3 | 229.8731 | 0.365 | 8740 | 0 |
| 1% loss, 26.4 ms RTT | 1 | 248.9013 | 0.337 | 8802 | 62 |
| 1% loss, 26.4 ms RTT | 2 | 259.0431 | 0.324 | 8835 | 95 |
| 1% loss, 26.4 ms RTT | 3 | 265.3814 | 0.316 | 8856 | 116 |
| 5% loss, 26.4 ms RTT | 1 | 365.0682 | 0.230 | 9188 | 448 |
| 5% loss, 26.4 ms RTT | 2 | 361.7937 | 0.232 | 9183 | 443 |
| 5% loss, 26.4 ms RTT | 3 | 368.9089 | 0.227 | 9195 | 455 |

All nine runs verified byte-identical.

Notes on Condition B:

- **The model predicts the measurement almost exactly.** Stop-and-wait moves one
  chunk per RTT, so predicted throughput is
  `1200 bytes × 8 / 0.0264 s = 0.364 Mbps`. Measured: 0.364, 0.364, 0.365 Mbps.
  Per-round-trip time from the run itself is 230.3377 s / 8740 = **26.35 ms**,
  against the 26.4 ms measured independently with the 11-round-trip probe.
- **Adding 26 ms of RTT costs a factor of 149.** 1.5461 s → 230.3377 s at 0%
  loss, on the same machine, moving the same file, with an identical packet
  count. Nothing about the bandwidth changed; only the time spent waiting did.
- **This is the entire case for milestone 3.** Throughput here is a function of
  RTT and chunk size and nothing else — it is completely insensitive to
  available bandwidth. A window of N packets in flight should multiply it by
  roughly N until some other limit binds.
- **Loss matters less here, proportionally.** At 0 ms RTT, 5% loss cost a factor
  of 89 (1.55 s → 138 s), because the 300 ms RTO dwarfed the 0.18 ms baseline.
  At 26.4 ms RTT it costs only a factor of 1.6 (230 s → 365 s), because the RTO
  is now merely ~11× the RTT rather than ~1700×. The fixed 300 ms RTO is
  badly mismatched to both cases, in opposite directions.
- **Three more genuine, non-simulated retransmissions were observed** (0% run 2:
  `retransmissions=1, simulated_drops=0`; 1% run 3: 116 vs 115; 5% run 2: 443
  vs 442). Consistent with the one seen in Condition A.

### Verbatim terminal output

Condition A, 5% loss, run 2 — client then server, unedited:

```
sending /tmp/.../scratchpad/data/bench10m.bin to 127.0.0.1:9432
  chunk=1200 rto=300ms simulate_loss=0.05 seed=1218004369
STATS elapsed_s=136.9026 throughput_mbps=0.613 bytes=10485760 chunks=8739 packets_sent=9177 retransmissions=437 simulated_drops=437 timeouts=437 seed=1218004369

swiftlink server on 0.0.0.0:9432 -> /tmp/.../scratchpad/data/recv_no-netem_0.05_2.bin
READY
STATS elapsed_s=136.9123 throughput_mbps=0.613 bytes=10485760 chunks=8739 packets_received=8740 duplicates=0 out_of_order=0
```

Condition B, 0% loss, run 1 — client, proxy, then server, unedited:

```
sending /tmp/.../scratchpad/data/bench10m.bin to 127.0.0.1:9803
  chunk=1200 rto=300ms simulate_loss=0 seed=1470199125
STATS elapsed_s=230.3377 throughput_mbps=0.364 bytes=10485760 chunks=8739 packets_sent=8740 retransmissions=0 simulated_drops=0 timeouts=0 seed=1470199125

delay_proxy :9803 -> :9802 one-way 12.5ms (RTT 25ms)
READY

swiftlink server on 0.0.0.0:9802 -> /tmp/.../scratchpad/data/recv_delay_0.00_1.bin
READY
STATS elapsed_s=230.3182 throughput_mbps=0.364 bytes=10485760 chunks=8739 packets_received=8740 duplicates=0 out_of_order=0
```

(Only the long scratchpad paths are elided, marked `/tmp/.../`. All numbers are
as printed.)

---

## Milestone 3 — Selective Repeat

Not yet measured; milestone 3 is not implemented.
