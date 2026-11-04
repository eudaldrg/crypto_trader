# Capture volume per symbol (2026-09-20)

Question: how much data does a capture produce, and how many symbols should the
first real capture cover? Measured from real captures in this repo.

| | rate | per day |
|---|---|---|
| Kraken `BTC/USD` `level3` alone | ~50 msg/s, ~17.8 KiB/s | ~1.5 GiB |
| Deribit `BTC-PERPETUAL` alone | ~9.6 msg/s, ~2.4 KiB/s | ~0.2 GiB |
| Kraken `BTC/USD` + `ETH/USD` (30 s sample) | ~28 KiB/s of journal | ~2.3 GiB |

Later hour-long measurements for sizing the journal ring: Kraken 108,573 and
Deribit 43,606 messages per hour.

Scaled against Kraken's 24 h trade counts (`BTC/USD` is about 6.2% of all-pair
trades, the top five about 21%) and Deribit's ~5,540 active instruments (~5,100
of them BTC/ETH options), the options were:

- (a) BTC only on both exchanges: ~2 GiB/day.
- (b) the top five per exchange: ~6 GiB/day.
- (c) everything: ~40 to 90 GiB/day (1 to 3 TB a month).

(c) buys no extra portfolio story over (b), for all of the cost, so (b) became
the target (`docs/architecture.md`). The ETH sample ran at several times
`BTC/USD`'s update rate, so (b) is an estimate to re-measure, not a budget.

Not done at this scope: subscribe pacing (needed past about 40 Kraken symbols at
depth 10) and connection sharding (past 200 symbols).
