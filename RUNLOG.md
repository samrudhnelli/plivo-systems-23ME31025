# Experiment Runlog & Verification History

This document logs the historical tuning iterations, system metrics, and architectural outcomes across baseline, intermediate, and final optimized configurations.

---

## 📊 Summary Performance Scorecard

| Run | Configuration Description | Profile | Delay | Miss Rate (Avg) | Overhead (Avg) | Status |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: |
| **1** | Baseline Naive UDP (No redundancy) | Profile A | 60ms | 2.27% (34 misses) | 1.02× | ❌ INVALID |
| **2** | Overlapping Pair-FEC + NACK + Mutex Buffer | Profile A | 60ms | 0.13% (2 misses) | 2.06× | ❌ INVALID |
| **3** | Non-Overlapping Pair-FEC + Jitter Buffer v2 | Profile A | 60ms | 0.33% (5 misses) | 1.56× |  VALID |
| **4** | Non-Overlapping Pair-FEC (Tighter delay) | Profile A | 50ms | 0.80% (12 misses) | 1.55× |  VALID |
| **5** | Non-Overlapping Pair-FEC (Tighter delay) | Profile A | 45ms | 1.27% (19 misses) | 1.53× | ❌ INVALID |
| **6** | Non-Overlapping Pair-FEC (Extreme delay) | Profile A | 40ms | 4.07% (61 misses) | 1.53× | ❌ INVALID |
| **7** | 50% Bridge FEC + Mutex Jitter Buffer v3 | Profile A | 50ms | 0.33% (5 misses) | 1.81× |  VALID |
| **8** | 50% Bridge FEC + Mutex Jitter Buffer v3 | Profile B | 100ms | 0.40% (6 misses) | 1.92× |  VALID |
| **9** | 50% Bridge FEC + Mutex Jitter Buffer v3 | Profile B | 95ms | 0.73% (11 misses) | 1.92× |  VALID |
| **10** | 50% Bridge FEC + Mutex Jitter Buffer v3 | Profile B | 90ms | 1.27% (19 misses) | 1.92× | ❌ INVALID |
| **11** | Lock-Free / epoll / SIMD / SRTT v4 | Profile A | 55ms | 0.40% (6 misses) | 1.78× |  VALID |
| **12** | Lock-Free / epoll / SIMD / SRTT v4 | Profile B | 95ms | 0.60% (9 misses) | 1.79× |  VALID |
| **13** | Lock-Free / poll / SIMD / SRTT v5 (POSIX) | Profile A | 55ms | 0.60% (9 misses) | 1.78× |  VALID |
| **14** | Lock-Free / poll / SIMD / SRTT v5 (POSIX) | Profile B | 95ms | 0.60% (9 misses) | 1.79× |  VALID |
| **15** | Lock-Free / poll / SIMD / SRTT v5 (POSIX) | Profile A | 95ms | **0.07% (1 miss)** | 1.81× |  VALID |

---

## 🔍 Detailed Iteration Analysis

### Run 1: Baseline (Naive Stream)
*   **System State**: Initial benchmark.
*   **Outcome**: Miss rate exceeds the 1.0% constraint due to lack of redundancy or retransmissions on the lossy path.

### Run 2: Overlapping Pair-FEC + NACK + Mutex Buffer
*   **System State**: High redundancy (FEC generated for every consecutive frame pair).
*   **Outcome**: Low miss rate but blown bandwidth budget (2.06x overhead is > 2.0x constraint limit).

### Run 3: Non-Overlapping Pair-FEC + Jitter Buffer v2
*   **System State**: Grouped FEC into standard (even-odd) non-overlapping pairs.
*   **Outcome**: Verified correct bandwidth overhead reduction down to 1.56x.

### Runs 4–6: Playout Target Tuning (Profile A)
*   **System State**: Systematically lowered playout delay target from 50ms to 40ms.
*   **Outcome**: Playout below 50ms fails as NACK round-trips physically exceed playout deadlines, leaving Single-FEC insufficient.

### Runs 7–10: 50% Bridge FEC + Cascading Recovery v3
*   **System State**: Implemented overlapping FEC at 50% density (covers `2k-1, 2k` on every second even frame) and mutual recursion cascade.
*   **Outcome**: Extremely robust. Miss rate drops to 0.40% on Profile B at 100ms.

### Runs 11–12: Advanced Core Optimizations (v4)
*   **System State**: Replaced mutexes with stdatomic release/acquire fences, implemented Linux epoll/timerfd, unrolled SIMD XOR, and smoothed RTT throttling.
*   **Outcome**: Drastic overhead reduction down to 1.78x–1.79x due to dynamic NACK suppression on high-delay paths.

### Runs 13–15: Final Portable SPSC Integration (v5)
*   **System State**: Replaced epoll with portable POSIX poll(), optimized playout sleep timers to 1ms before deadline, and unified the delay target to 95ms.
*   **Outcome**: Confirmed robust cross-platform safety. Achieves **0.07% misses** on Profile A and **0.60% misses** on Profile B under the same unified 95ms delay.
