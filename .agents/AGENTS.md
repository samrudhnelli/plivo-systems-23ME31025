# Flaky Network Challenge — Agent Context

## Project Overview
Real-time 160-byte audio frame transport over a simulated hostile UDP network.
Goal: ≤1% deadline-miss rate, ≤2.0× bandwidth overhead, minimize DELAY_MS.

## Architecture Summary

### Port Layout (all 127.0.0.1, UDP)
```
Harness Source →[47010]→ SENDER →[47001]→ RELAY →[47002]→ RECEIVER →[47020]→ Harness Player
                                  [47004]← RELAY ←[47003]←
                                         (feedback/NACK path)
```

### Wire Format (our custom protocol, between sender↔relay↔receiver)
- **DATA (0x01)**: `[1B type][2B BE seq][160B payload]` = 163 bytes
- **FEC  (0x02)**: `[1B type][2B BE seq][160B XOR payload]` = 163 bytes (seq = first of pair)
- **RETX (0x03)**: Same as DATA, sent in response to NACK
- **NACK (0x04)**: `[1B type][2B BE seq]` = 3 bytes (receiver→sender via relay)

### Harness Format (fixed, do NOT change)
- `[4B big-endian uint32 seq][160B payload]` = 164 bytes
- Frame i arrives at sender at T0 + i×20ms
- Frame i deadline at player: T0 + DELAY_MS + i×20ms

## Network Profiles

### Profile A ("mild")
- Loss: 2%, Delay: 10–40ms, Dup: 0.5%
- No burst loss model
- RTT through relay: 20–80ms

### Profile B ("moderate")  
- Loss: 5%, Delay: 20–80ms, Dup: 1%
- No burst loss model in known profiles, but grading uses UNSEEN profiles
- RTT through relay: 40–160ms

### Unknown Grading Profiles
- May include burst_loss (Gilbert-Elliott model: p_enter, p_exit, p_loss_in_burst)
- May include spike delays (prob + extra_ms)
- Relay code in relay.py shows all possible impairment types

## Design Decisions

### Why Pair-FEC (XOR)?
- For non-overlapping pairs (0,1),(2,3),...: 1 FEC per 2 frames = 1.5× packets
- At 163 bytes/packet: 2250 packets × 163 = 366,750 bytes → 1.53× overhead
- Recovery: if either DATA in a pair is lost but FEC + other DATA survive, XOR recovers
- P(unrecoverable, 2% loss) ≈ 0.12% per frame ✓
- P(unrecoverable, 5% loss) ≈ 0.49% per frame — tight but workable

### Why NACKs Have Limited Value
- NACK must traverse relay TWICE (receiver→relay→sender, then sender→relay→receiver)
- RTT = 2 × relay_delay. Profile A: 20–80ms. Profile B: 40–160ms.
- At DELAY_MS=50, only ~10ms headroom after max relay delay — NACKs rarely help
- At DELAY_MS=45, zero NACKs completed successfully in testing
- NACKs are a safety net, not primary recovery

### Bandwidth Budget
- Raw stream: 1500 frames × 160 bytes = 240,000 bytes
- Cap: 2.0× = 480,000 bytes total (up + down)
- Current usage: ~367K up + ~100B down = 1.53×
- **Headroom: ~113KB = ~690 additional packets**
- This headroom can fund sliding-window FEC (bridge pairs)

### Jitter Buffer Design
- 8192-slot circular buffer (BUF_MASK = 8191)
- Deduplication via received-flag array
- FEC recovery triggered on both DATA and FEC arrival
- Playout thread delivers 1ms before deadline (scheduling margin)

## Experimental Results (locked in)

| # | Profile | Delay | Misses | Overhead | Valid | Notes |
|---|---------|-------|--------|----------|-------|-------|
| 1 | A | 60ms | 2.27% (34) | 1.02× | ❌ | Baseline — no redundancy |
| 2 | A | 60ms | 0.13% (2) | 2.06× | ❌ | FEC every consecutive pair (overlapping) — overhead blown |
| 3 | A | 60ms | 0.33% (5) | 1.56× | ✅ | Non-overlapping pair FEC — FIRST VALID |
| 4 | A | 50ms | 0.80% (12) | 1.55× | ✅ | Tighter delay — still valid but close |
| 5 | A | 45ms | 1.27% (19) | 1.53× | ❌ | Too tight, NACKs can't complete |
| 6 | A | 40ms | 4.07% (61) | 1.53× | ❌ | Way too tight |
| 7 | A | 50ms | 0.33% (5) | 1.81× | ✅ | 50% Bridge FEC + Cascading Recovery — extremely robust |

## Key Files
- `sender.c` — dual FEC sender (50% Bridge FEC) with NACK retransmission (pthread for feedback listener)
- `receiver.c` — Jitter buffer + cascading FEC recovery + NACK sender + playout thread
- `Makefile` — builds with `-lpthread`
- `relay.py` — DO NOT MODIFY. Shows all impairment types (loss, burst_loss, delay, spike, dup)
- `endpoints.py` — DO NOT MODIFY. Source + player threads.
- `score.py` — DO NOT MODIFY. Scoring formula.
- `common.py` — Port constants, frame_payload generator.

## Critical Implementation Notes

### Sender Threading
- Main thread: receives harness frames on 47010, writes to ring buffer, sends DATA+FEC to relay on 47001
- Feedback thread: receives NACKs on 47004, reads from ring buffer and retransmits
- Ring buffer: 4096 slots, SPSC lock-free (`stdatomic.h` memory barriers)

### Receiver Threading
- Main thread: POSIX `poll()` event-driven loop with MSG_DONTWAIT non-blocking read drains, stores in jitter buffer, sends NACKs on 47003
- Playout thread: delivers at T0 + DELAY_MS + i×20ms to player on 47020. Employs hardware-specific pause/yield spin-wait (`CPU_PAUSE()`) for sub-millisecond precision.
- Jitter buffer: 8192 slots, SPSC lock-free using `stdatomic.h` release-acquire semantics

### Dual FEC Pairing Scheme
- Standard Pair-FEC (0x02): Covers non-overlapping pairs `(2k, 2k+1)`. Sent on odd frames.
- Bridge Pair-FEC (0x05): Covers overlapping pairs `(2k-1, 2k)`. Sent at 50% density on even frames (when `(prev_seq & 3) == 1`).
- This sample density yields exactly 1.75× baseline packets, keeping total overhead (including headers and NACKs/RETX) strictly under 2.0×.

### Cascading Recovery Algorithm
- Implemented as mutual recursion between `try_fec_recovery` and `trigger_recovery_cascade`.
- When any frame is received or successfully recovered, `trigger_recovery_cascade` immediately tests both its standard FEC pair and its bridge FEC pair.
- If either check recovers a neighbor frame, it triggers another recovery cascade for that neighbor, resolving multi-packet drops sequentially in a single step.

## Failure Modes to Watch
1. **Burst loss** — consecutive frame drops > 2 defeats dual-pair FEC (though cascading mitigates short bursts)
2. **Spike delays** — relay can add extra_ms spikes pushing packets past deadline
3. **Clock drift** — playout thread uses CLOCK_REALTIME, should match harness
4. **Buffer wraparound** — 8192 slots is fine for 1500 frames but watch for seq collisions
5. **Playout oversleep** — resolved by sleeping 0.5ms early and spinning with hardware yield instructions
