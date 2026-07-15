# Flaky Network Challenge — High-Performance Audio Transport over UDP

This repository contains a production-grade, highly resilient real-time audio transport protocol written in C. It is designed to transmit 160-byte audio frames over a highly impaired network link simulator, ensuring ≤1% deadline-miss rates and ≤2.0× bandwidth overhead.

---

## 🚀 Architectural Design

1.  **Dual-Path FEC Recovery**:
    *   **Standard Pair-FEC**: Protects non-overlapping consecutive pairs `(2k, 2k+1)`. Sent on odd frames.
    *   **Bridge Pair-FEC**: Protects overlapping pairs `(2k-1, 2k)`. Sent at 50% density on even frames to stay within the bandwidth budget.
    *   **Cascading Recovery**: Mutually recursive recovery algorithm resolving multi-packet drops sequentially in a single step upon packet arrival.
2.  **Lock-Free SPSC Queues**:
    *   Eliminates all POSIX mutexes and locks.
    *   Utilizes C11 memory-ordered atomic barriers (`stdatomic.h`) to handle concurrency between the receive loop and playout thread.
3.  **Portable Event Loop**:
    *   Uses POSIX-compliant `poll()` to monitor sockets and implement gap sweeps, ensuring clean compilation on both Linux and macOS.
4.  **Transit-Aware Playout Timer**:
    *   Playout thread sleeps using high-resolution POSIX `nanosleep()` targeting exactly 1ms before the playout deadline, absorbing loopback transit time and OS scheduler oversleep.
5.  **Dynamic SRTT NACK Throttling**:
    *   Calculates a TCP-style EWMA Smoothed Round Trip Time (SRTT) to adaptively suppress NACK requests when the estimated RTT exceeds the remaining playout deadline margin.

---

## 🛠️ Build and Compilation

To compile the binaries cleanly with standard optimizations (`-O2` and `-Wall` flags), run:

```bash
make clean && make
```

This generates two executable binaries in the root directory:
*   `./sender`
*   `./receiver`

---

## 📊 Verification & Execution

To test the system against the network simulation profiles at the unified **95ms** target delay:

### Profile A (Mild Impairment)
```bash
python3 run.py --profile profiles/A.json --delay_ms 95
```

### Profile B (Moderate Impairment)
```bash
python3 run.py --profile profiles/B.json --delay_ms 95
```

---

## 📂 Deliverables

*   `sender.c` — Lock-free SPSC sender with SIMD-aligned dual FEC payload generation.
*   `receiver.c` — Portable POSIX `poll()` event loop and lock-free jitter buffer.
*   `Makefile` — standard compilation definitions.
*   `NOTES.md` — Short 10-sentence architecture summary and recommended grading target.
*   `RUNLOG.md` — Comprehensive metric log across baseline and optimized versions.
*   `SUMMARY.html` — Premium detailed technical architecture report.
