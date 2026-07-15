# RUNLOG — Experiment Log

## Run 1: Baseline (naive sender/receiver)
- **Profile:** A (2% loss, 10–40ms delay, 0.5% dup)
- **DELAY_MS:** 60
- **Result:** INVALID
- **Miss rate:** 2.27% (34/1500)
- **Overhead:** 1.02×
- **Notes:** Baseline forwards each frame once with no redundancy. All losses become misses. No FEC, no retransmission, no jitter buffer.

## Run 2: Pair-FEC + NACK + Jitter Buffer v1
- **Profile:** A
- **DELAY_MS:** 60
- **Result:** INVALID (Overhead blown)
- **Miss rate:** 0.13% (2/1500)
- **Overhead:** 2.06×
- **Notes:** Complete rewrite of sender and receiver implementing standard Pair-FEC (overlapping consecutive pairs). While the miss rate is low, generating FEC on every frame exceeded the 2.0x limit.

## Run 3: Pair-FEC + NACK + Jitter Buffer v2
- **Profile:** A
- **DELAY_MS:** 60
- **Result:** VALID
- **Miss rate:** 0.33% (5/1500)
- **Overhead:** 1.56×
- **Notes:** Fixed FEC logic to only generate FEC on non-overlapping pairs (even-odd pairs). Brought overhead down to 1.56x, successfully validating the run.

## Run 4: Pair-FEC v2 (Reduced Delay)
- **Profile:** A
- **DELAY_MS:** 50
- **Result:** VALID
- **Miss rate:** 0.80% (12/1500)
- **Overhead:** 1.55×
- **Notes:** Reduced DELAY_MS to 50ms. Valid but close to the 1.0% limit. At 50ms, NACKs are starting to arrive too late due to relay round-trip delays, leaving FEC as the main recovery mechanism.

## Run 5: Pair-FEC v2 (Reduced Delay)
- **Profile:** A
- **DELAY_MS:** 45
- **Result:** INVALID
- **Miss rate:** 1.27% (19/1500)
- **Overhead:** 1.53×
- **Notes:** Attempted 45ms playout delay. Playout is too tight, making NACKs entirely useless and single FEC alone insufficient to cover all losses.

## Run 6: Pair-FEC v2 (Reduced Delay)
- **Profile:** A
- **DELAY_MS:** 40
- **Result:** INVALID
- **Miss rate:** 4.07% (61/1500)
- **Overhead:** 1.53×
- **Notes:** Playout delay is at the minimum relay delay limit. Massive packet late arrivals.

## Run 7: 50% Bridge FEC + Cascading Recovery v3
- **Profile:** A
- **DELAY_MS:** 50
- **Result:** VALID
- **Miss rate:** 0.33% (5/1500)
- **Overhead:** 1.81×
- **Notes:** Implemented 50% density Bridge FEC (XOR on overlapping pairs sent on every second even frame) + a mutually recursive cascading recovery engine. Misses dropped to 0.33% with plenty of overhead headroom (1.81x).

## Run 8: 50% Bridge FEC + Cascading Recovery v3
- **Profile:** B (5% loss, 20-80ms delay, 1% dup)
- **DELAY_MS:** 100
- **Result:** VALID
- **Miss rate:** 0.40% (6/1500)
- **Overhead:** 1.92×
- **Notes:** Tested on the more hostile Profile B. Successfully validated with very low misses.

## Run 9: 50% Bridge FEC + Cascading Recovery v3
- **Profile:** B
- **DELAY_MS:** 95
- **Result:** VALID
- **Miss rate:** 0.73% (11/1500)
- **Overhead:** 1.92×
- **Notes:** Pushed Profile B delay down to 95ms. The cascading FEC successfully recovered consecutive losses.

## Run 10: 50% Bridge FEC + Cascading Recovery v3
- **Profile:** B
- **DELAY_MS:** 90
- **Result:** INVALID
- **Miss rate:** 1.27% (19/1500)
- **Overhead:** 1.92×
- **Notes:** Playout delay of 90ms is too close to average Profile B round-trip time.

## Run 11: Lock-Free / epoll / SIMD / SRTT v4
- **Profile:** A
- **DELAY_MS:** 55
- **Result:** VALID
- **Miss rate:** 0.40% (6/1500)
- **Overhead:** 1.78×
- **Notes:** epoll/timerfd and atomic lock-free version. Profile A is robust at 55ms.

## Run 12: Lock-Free / epoll / SIMD / SRTT v4
- **Profile:** B
- **DELAY_MS:** 95
- **Result:** VALID
- **Miss rate:** 0.60% (9/1500)
- **Overhead:** 1.79×
- **Notes:** epoll/timerfd and atomic lock-free version. Profile B is robust at 95ms.

## Run 13: Lock-Free / poll / SIMD / SRTT v5 (Portable Event Loop)
- **Profile:** A
- **DELAY_MS:** 55
- **Result:** VALID
- **Miss rate:** 0.60% (9/1500)
- **Overhead:** 1.78×
- **Notes:** Portable POSIX poll() receive loop and atomic lock-free sender-receiver version. Meets the 1% cap at 55ms.

## Run 14: Lock-Free / poll / SIMD / SRTT v5 (Portable Event Loop)
- **Profile:** B
- **DELAY_MS:** 95
- **Result:** VALID
- **Miss rate:** 0.60% (9/1500)
- **Overhead:** 1.79×
- **Notes:** Portable POSIX poll() receive loop and atomic lock-free sender-receiver version. Meets the 1% cap at 95ms.

## Run 15: Lock-Free / poll / SIMD / SRTT v5 (Unified Playout Target)
- **Profile:** A
- **DELAY_MS:** 95
- **Result:** VALID
- **Miss rate:** 0.07% (1/1500)
- **Overhead:** 1.81×
- **Notes:** Playout delay matched to the unified 95ms target delay. Achieves an exceptionally low miss rate of 0.07% on Profile A.
