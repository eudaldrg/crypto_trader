---
aliases: [capture, run capture, record, journal a session]
---

# Running a capture

How to record a live session to a journal with the two feed-handler binaries,
and how to check one without ever printing what is in it.

## Run

```bash
cmake --build build/release
./build/release/bin/kraken_feed_handler  --config config/feed_handler.toml
./build/release/bin/deribit_feed_handler --config config/feed_handler.toml
```

Each runs until SIGINT/SIGTERM and shuts down cleanly (journal flushed, Deribit
logged out). `--config` is required. Copy `config/feed_handler.toml` and edit
its `symbols` to change what is captured; the schema is in
`docs/modules/feed-handler.md`.

The credentials the config names (`KRAKEN_API_KEY`, `KRAKEN_API_SECRET`,
`DERIBIT_TESTNET_CLIENT_ID`, `DERIBIT_TESTNET_CLIENT_SECRET`) must be in the
environment of the process. Do not `source` or print `experiments/.env` to get
them there: use a small launcher that reads the file and `exec`s the binary
with only that exchange's variables, and prints variable names, never values,
when one is missing.

## Where it goes

Journals go to the config's `journal_dir`, state (Kraken's nonce mark) to
`state_dir`, both relative to the working directory. **A long capture belongs
outside the repo, in `~/Temp` (not `/tmp`)**: set both to absolute paths there.
`journal/` and `*.journal` are gitignored, but a capture is large and can hold
the auth handshake, so it should not sit in the checkout at all
(`AGENTS.md`).

## Check it without printing it

Never `cat`, `head` or `strings` a journal. Count instead:

```bash
ls -la <journal_dir>                                  # one file per connection incarnation
grep -a -o '"type":"snapshot"' <file> | wc -l          # Kraken: one per symbol per connection
grep -a -o '55=BTC-PERPETUAL' <file> | wc -l           # Deribit: records mentioning a symbol
```

The run's own log is the first check: Kraken prints `subscribed to level3 SYM
(k/N)` per symbol and Deribit `received a MarketDataSnapshotFullRefresh for
SYM`. A symbol missing from those lines is a partial capture that no error
reports. `kraken_journal_dump` (`docs/modules/order-book.md`) replays a Kraken
journal through the real book and reports checksum mismatches.

## Sizing

Roughly 1.5 GiB/day for `BTC/USD` on Kraken and 0.2 GiB/day for
`BTC-PERPETUAL` on Deribit, and other symbols differ several-fold; the numbers
and the 5-10 symbols per exchange scope are in `decisions/0004`.
