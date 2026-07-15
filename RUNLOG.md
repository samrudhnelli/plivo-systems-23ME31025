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

## Run 11: 50% Bridge FEC + Cascading Recovery v3
- **Profile:** B
- **DELAY_MS:** 85
- **Result:** INVALID
- **Miss rate:** 1.40% (21/1500)
- **Overhead:** 1.92x
- **Notes:** Playout delay too tight.
