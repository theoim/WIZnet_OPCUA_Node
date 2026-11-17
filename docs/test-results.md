# Test results

[한국어](test-results_kor.md) · [README](../README.md)

Measurements behind the numbers in the README. Each section gives the date,
board and conditions. Numbers apply to those conditions only.

## 1. 24-hour soak — W6300-EVB-Pico2

2026-11-09 to 11-10. Security build (commit 5c87e96), two clients: one
Basic256Sha256 Sign & Encrypt, one None. Tool: `tools/opcua_soak_test.py`.

| Load | Value |
|---|---|
| UART input | 10 lines/s for 24 h |
| Normal connect/disconnect | every 120 s |
| Client killed without closing | every 600 s |
| Link outage (cable down 20 s) | every 2 h |

| Check | Result |
|---|---|
| Reboots | **0** |
| UART lines parsed | **864,002 / 864,002**, 0 errors |
| Values seen by clients | 169,416 snapshots, 0 bad, no duplicates or reordering |
| Unexpected client losses | **0** |
| Normal connects | 577 / 577 |
| Killed sessions reclaimed | 131 / 131, within 30–36 s |
| Link outage recovery | 12 / 12, within 45.3 s |
| Lowest free heap | 299,008 B |
| Free heap, start → end | 333,504 → 327,740 B (−5,764 B) |

**The heap drop is a bounded queue, not a leak.** A 70-minute follow-up with
link outages only (22 outages, 3 minutes apart) showed the same drop of about
500 B per outage for the first 15 outages, then no further drop. Responses sent
while the link is down are never acknowledged, so they stay in the session's
retransmission queue. The queue holds at most 16 entries per session; when it
was full the server logged "retransmission queue overflow" and evicted the
oldest. The memory returns when the session ends.

No action is needed. Where the link drops often, budget up to about 12 KB of heap
per session for this queue (4 KB on RP2040); the limit is the CMake option
`OPCUA_RETRANSMIT_MAX_HEAP`.

## 2. Secure handshake impact — W6300-EVB-Pico2

2026-11-11. Security build, Basic256Sha256. Tool: `tools/opcua_handshake_impact.py`.

| Measurement | Value |
|---|---|
| One secure channel open (OPN) | 1,233 ms, of which the private-key operation 1,156 ms |
| Longest wait of another session's Read during it | 1.2 s (normally p99 6 ms) |
| Longest gap in another session's notifications | 1.33 s (normally p99 104 ms) |
| Two clients connecting at once | 2.4 s / 2.5 s |
| Board TCP ACK delay during the handshake | p99 ≤ 0.9 ms |

TCP connections stay up: the W6300 sends TCP ACKs from the chip while the MCU is
busy. A client with a 1 s watchdog may time out once during a handshake; with
5 s it did not.

## 3. RSA and ECC cost by MCU

Same Mbed TLS build on both parts, measured with a benchmark build in the
development tree (not part of this repository).

| | RSA-2048 handshake (Basic256Sha256) | ECC P-256 handshake |
|---|---|---|
| RP2040 @200 MHz (overclocked; rated 133 MHz) | **5,050 ms** | 384 ms |
| RP2350 @150 MHz (stock) | **1,229 ms** | 264 ms |

The RP2350's Cortex-M33 has a 32×32→64-bit multiply; the RP2040's Cortex-M0+
does not. ECC is cheaper on both, but common OPC UA clients connect with
Basic256Sha256, so RSA is the policy that matters.

## 4. Capacity — W6300-EVB-Pico2

2026-11-02. Four clients × 100 monitored items: free heap 247,968 B, lowest
176,128 B. The 401st item is answered `Bad_TooManyMonitoredItems`.

## 5. Response time — W55RP20-EVB-Pico (None)

2026-11-02, LAN, None policy.

| Measurement | Value |
|---|---|
| Connect | 15–18 ms |
| Read | p50 1.9 ms, p99 3–5 ms (about 520 reads/s) |
| 100 monitored items | 1,000 notifications/s, all received |
