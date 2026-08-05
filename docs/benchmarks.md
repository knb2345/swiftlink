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

Measured at commit `e768f12` (the M3 tree), Debug build, same 10 MiB file and
same machine as above. Window sizes 1, 8 and 32 only: 4, 16 and 64 were dropped
at the user's request to keep the run short. Window 1 is included because it
makes the Selective Repeat code reproduce stop-and-wait exactly, so the
comparison runs through one code path rather than two.

### Condition A: loopback, no artificial delay

| window | loss | run | elapsed s | throughput Mbps | packets sent | retransmissions |
|---|---|---|---|---|---|---|
| 1 | 0% | 1 | 1.2057 | 69.572 | 8740 | 0 |
| 1 | 0% | 2 | 1.2320 | 68.091 | 8740 | 0 |
| 1 | 0% | 3 | 1.4913 | 56.249 | 8740 | 0 |
| 1 | 1% | 1 | 31.3118 | 2.679 | 8835 | 95 |
| 1 | 1% | 2 | 24.4321 | 3.433 | 8814 | 74 |
| 1 | 1% | 3 | 34.5649 | 2.427 | 8846 | 106 |
| 1 | 5% | 1 | 135.5456 | 0.619 | 9174 | 434 |
| 1 | 5% | 2 | 142.8843 | 0.587 | 9200 | 460 |
| 1 | 5% | 3 | 141.0298 | 0.595 | 9191 | 451 |
| 8 | 0% | 1 | 0.1177 | 712.993 | 8740 | 0 |
| 8 | 0% | 2 | 0.1010 | 830.605 | 8740 | 0 |
| 8 | 0% | 3 | 0.1199 | 699.778 | 8740 | 0 |
| 8 | 1% | 1 | 27.3586 | 3.066 | 8838 | 98 |
| 8 | 1% | 2 | 27.2903 | 3.074 | 8834 | 94 |
| 8 | 1% | 3 | 30.4446 | 2.755 | 8846 | 106 |
| 8 | 5% | 1 | 109.0075 | 0.770 | 9206 | 466 |
| 8 | 5% | 2 | 106.4077 | 0.788 | 9196 | 456 |
| 8 | 5% | 3 | 102.9282 | 0.815 | 9198 | 458 |
| 32 | 0% | 1 | 0.1825 | 459.672 | 8740 | 0 |
| 32 | 0% | 2 | 0.1261 | 665.151 | 8740 | 0 |
| 32 | 0% | 3 | 0.0937 | 895.694 | 8740 | 0 |
| 32 | 1% | 1 | 18.9509 | 4.426 | 8823 | 83 |
| 32 | 1% | 2 | 19.8900 | 4.217 | 8831 | 91 |
| 32 | 1% | 3 | 18.9669 | 4.423 | 8828 | 88 |
| 32 | 5% | 1 | 56.1275 | 1.495 | 9183 | 443 |
| 32 | 5% | 2 | 57.0588 | 1.470 | 9178 | 438 |
| 32 | 5% | 3 | 54.2715 | 1.546 | 9171 | 431 |

All 27 runs verified byte-identical.

### Condition B: 26.4 ms RTT (delay proxy, see the M2 note)

Three runs at window 8 and 32; **one** run at window 1, because each such run
takes nearly four minutes and the M2 table already records three at that
setting. Single runs are labelled as such rather than presented as a mean.

| window | loss | run | elapsed s | throughput Mbps | packets sent | retransmissions |
|---|---|---|---|---|---|---|
| 1 | 0% | 1 (single run) | 227.0879 | 0.369 | 8741 | 1 |
| 8 | 0% | 1 | 28.3185 | 2.962 | 8740 | 0 |
| 8 | 0% | 2 | 28.5250 | 2.941 | 8740 | 0 |
| 8 | 0% | 3 | 28.5132 | 2.942 | 8740 | 0 |
| 32 | 0% | 1 | 7.1461 | 11.739 | 8740 | 0 |
| 32 | 0% | 2 | 7.1392 | 11.750 | 8740 | 0 |
| 32 | 0% | 3 | 7.1750 | 11.691 | 8740 | 0 |

The loss columns of Condition B were **not measured** — the run was trimmed for
time, and estimating them would be fabrication.

### What the window buys

Throughput on an RTT-bound path should be `window x chunk / RTT`. Against the
measured 26.4 ms RTT and 1200-byte chunks:

| window | predicted Mbps | measured Mbps | error |
|---|---|---|---|
| 1 | 0.364 | 0.369 | +1.4% |
| 8 | 2.909 | 2.941–2.962 | +1.1% to +1.8% |
| 32 | 11.636 | 11.691–11.750 | +0.5% to +1.0% |

Scaling is linear in the window size to within 2% across a 32x range: window 8
is 8.0x window 1, window 32 is 31.8x. That is the entire thesis of milestone 3,
and it is measured rather than asserted.

### Bandwidth-delay product

BDP = bandwidth x RTT. Using the highest throughput this machine actually
sustained on loopback with no artificial delay (895.694 Mbps, window 32 run 3)
as the path capacity:

```
BDP = 895.694e6 bit/s x 0.0264 s = 23.6 Mbit = 2.96 MB
    = 2.96e6 / 1200 = about 2460 packets in flight to fill the pipe
```

**Every window size tested — 1, 8 and 32 — is far below the BDP of ~2460
packets.** That is precisely why throughput scaled linearly with no sign of
levelling off: none of them came close to filling the path, so each extra
packet in flight bought its full share of bandwidth. The curve would only bend
as the window approached ~2460, at which point the link, not the RTT, becomes
the limit.

That figure is theoretical for this setup, though: see the receive-buffer
ceiling below, which binds long before the BDP does.

---

## Milestone 6 — full system, and two effects worth knowing

Measured at the M6 tree with `scripts/benchmark.sh`, 10 MiB, loopback, 0% loss.
This is the complete system: handshake, per-packet CRC32, end-to-end SHA-256,
and the epoll server.

| build | window | run | elapsed s | throughput Mbps | retransmissions |
|---|---|---|---|---|---|
| Debug (`-O0`) | 1 | 1 | 0.8991 | 93.295 | 1 |
| Debug | 1 | 2 | 0.9560 | 87.751 | 1 |
| Debug | 8 | 1 | 0.4565 | 183.746 | 1 |
| Debug | 8 | 2 | 0.4757 | 176.333 | 1 |
| Debug | 32 | 1 | 0.4047 | 207.287 | 0 |
| Debug | 32 | 2 | 0.4415 | 190.019 | 0 |
| Release (`-O3`) | 1 | 1 | 0.6227 | 134.720 | 0 |
| Release | 1 | 2 | 0.5381 | 155.897 | 0 |
| Release | 8 | 1 | 0.1400 | 599.256 | 0 |
| Release | 8 | 2 | 0.1305 | 642.846 | 0 |
| Release | 32 | 1 | 0.1230 | 681.850 | 0 |
| Release | 32 | 2 | 0.1005 | 835.055 | 0 |

### Effect 1: the Debug build is dominated by unoptimised crypto

Window 32 measured 207 Mbps in Debug against 835 Mbps in Release — a 4x gap
that is entirely explained rather than hand-waved. Timing the primitives
directly on 10 MiB:

| | `-O0` | `-O2` |
|---|---|---|
| SHA-256 | 355.5 ms (28 MB/s) | 55.8 ms (179 MB/s) |
| CRC-32 | 67.6 ms (148 MB/s) | 35.6 ms (281 MB/s) |

The receiver hashes the completed file *inside* the window the client is
timing, so the Debug SHA-256 alone adds ~355 ms to every transfer. The Debug
window-8 time was 0.4565 s against M3's 0.1010 s — a gap of 0.356 s, matching
the measured SHA-256 cost almost exactly. Release throughput (835 Mbps) is back
in line with M3's pre-integrity numbers (895 Mbps).

Lesson worth stating plainly: `CMAKE_BUILD_TYPE` defaults to Debug in this
project, and any benchmark that forgets to set Release is really measuring
`-O0` SHA-256.

### Effect 2: window 128 collapses — the receive buffer, not the network

| build | window | run | elapsed s | throughput Mbps | packets sent | retransmissions |
|---|---|---|---|---|---|---|
| Debug | 128 | 1 | 4.6346 | 18.100 | 9188 | 447 |
| Debug | 128 | 2 | 3.9523 | 21.225 | 9148 | 407 |
| Release | 128 | 1 | 8.2187 | 10.207 | 9704 | 963 |
| Release | 128 | 2 | 6.5989 | 12.712 | 9377 | 636 |

Hundreds of retransmissions at **zero simulated loss**, and a 7x throughput
collapse against window 32. Nothing was dropping these packets except the
kernel: the receiver's socket buffer is `net.core.rmem_max = 212992` bytes, and
the kernel charges each queued datagram roughly its payload plus several
hundred bytes of `sk_buff` overhead. At about 2 KB charged per 1240-byte
packet, the queue holds on the order of 100 packets. A window of 128 overruns
it, the kernel discards the excess, and the sender's retransmission timers
recover them one 300 ms RTO at a time.

This is the real ceiling on window size for this configuration, and it binds far
earlier than the ~2460-packet BDP computed above. Raising it means
`SO_RCVBUF`/`net.core.rmem_max`, which is listed in docs/todo.md — it needs
root, so it was not attempted here rather than being guessed at.
