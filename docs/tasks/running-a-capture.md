---
aliases: [capture, run capture, record, journal a session]
---

# Running a capture

How to record a live session to a journal with the `feed_handler` binary (with
order books optionally running alongside), and how to check one without ever
printing what is in it.

## Run

```bash
cmake --build build/release
./build/release/bin/feed_handler --config config/feed_handler.toml
./build/release/bin/feed_handler --config config/feed_handler.toml --exchange kraken
./build/release/bin/feed_handler --config config/feed_handler.toml --only kraken-btc-usd
```

One binary runs every `[[connections]]` entry, across both exchanges, until
SIGINT/SIGTERM, then shuts down cleanly (journals flushed, Deribit logged out).
`--config` is required; `--exchange` and `--only` narrow the set, and a filter
that matches nothing is an error. Exit codes: 0 clean, 1 a connection went
fatal (the log names it), 2 a startup, config, selection or credential error. A
second Ctrl-C during shutdown exits at once.

Copy `config/feed_handler.toml` and edit its `symbols` to change what is
captured; the schema is in `docs/modules/feed-handler.md`.

The credentials the config names (`KRAKEN_API_KEY`, `KRAKEN_API_SECRET`,
`DERIBIT_TESTNET_CLIENT_ID`, `DERIBIT_TESTNET_CLIENT_SECRET`) must be in the
environment of the process, for every entry that runs. A missing one is an
error naming it, and no entry is ever skipped for lack of credentials. Do not
`source` or print `experiments/.env` to get them there: use a small launcher
that reads the file and `exec`s the binary with only that exchange's variables,
and prints variable names, never values, when one is missing. To run one
exchange under such a launcher, pass `--exchange` as well: the flag does not
restrict credentials, the launcher's environment does, but without it the
process would ask for the other exchange's variables and refuse to start.

## Order books (optional)

`order_books = true` in the config also builds live books from the same frames, on
one extra thread; the default is off and the journals are identical either way. To
turn it on, the config also needs each Kraken connection's `depth` (default 10) and
each Deribit connection's `price_decimals` and `quantity_decimals`, or the process
refuses to start with exit code 2 naming what is missing. Books never fail a capture:
a Kraken symbol that the `AssetPairs` lookup does not know is captured without
books, and the log says `order books off for this connection, capture continues
without them`. Look for `order books on` per connection to confirm they are running.
Keys and behavior are in `docs/modules/feed-handler.md`.

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
ls -la <journal_dir>                                  # one file per connect_id (per connection)
grep -a -o '"type":"snapshot"' <file> | wc -l          # Kraken: one per symbol per connection
grep -a -o '55=BTC-PERPETUAL' <file> | wc -l           # Deribit: records mentioning a symbol
```

The run's own log is the first check. At exit it prints one summary line per
connection (`captured N messages across N connect(s), N journal records, ...`) and,
with books on, one `books:` line per connection. A healthy books line has `0 dropped`,
`0 parse errors`, `0 apply errors` and `integrity issues: none`; anything else means a
book desynced, and it stays so until that connection reconnects. A journal failure
(`journal failed: ...`) ends the capture with exit code 1. Kraken prints `subscribed to level3 SYM
(k/N)` per symbol and Deribit `received a MarketDataSnapshotFullRefresh for
SYM`. A symbol missing from those lines is a partial capture that no error
reports. `journal_replay` (`docs/modules/order-book.md`) replays a journal, of either exchange,
through the real books and reports counts and integrity issues, so a capture can be
checked after the fact whether or not books ran live; it takes the instrument's
decimals as flags. `kraken_journal_dump` replays one Kraken symbol and reports
checksum mismatches.

## Sizing

Roughly 1.5 GiB/day for `BTC/USD` on Kraken and 0.2 GiB/day for
`BTC-PERPETUAL` on Deribit, and other symbols differ several-fold; the numbers
and the 5-10 symbols per exchange scope are in `decisions/0004`.
