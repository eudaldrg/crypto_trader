---
aliases: [feed handler, feed-handler, capture, config, connections, journal thread, order_books]
sources: [src/feed_handler/**, config/**, src/book_adapter/book_service.*, src/book_adapter/book_wiring.*, src/book_adapter/ring_sink.h]
decisions: [decisions/0001-feed-source-selection.md, decisions/0004-feed-handler-architecture.md, decisions/0008-book-adapter-and-event-rings.md]
---

# Feed handler

The capture side of the pipeline: one `feed_handler` binary connects to every
configured exchange, subscribes to the configured symbols, and journals every
inbound wire message verbatim, and, with `order_books = true`, also feeds them into
live order books. Which connections to open is a TOML file passed with
`--config`; the design rationale is in `decisions/0004` (and `decisions/0008` for
the books), and this is what the file means and what to watch for.

## One connection, one socket, one journal

Each `[[connections]]` entry is one socket, one subscribe and one journal file
sequence. The one binary runs every entry, across exchanges:

```
feed_handler --config config/feed_handler.toml
feed_handler --config config/feed_handler.toml --exchange kraken
feed_handler --config config/feed_handler.toml --only kraken-btc-usd,deribit-btc-perp
```

`--exchange kraken|deribit` and `--only <id>[,<id>...]` (repeatable) narrow the
set, and together they narrow it further. An unknown id, or a filter that leaves
nothing, is an error rather than an empty capture.

### Credentials and `--exchange`

`--exchange` is **not** a credential control. Least privilege comes from the
environment the process is started in (the capture launcher exports only one
exchange's variables); the flag only stops the process asking for variables it
will not have. Every selected entry needs its variables set, and a missing one
is an error naming every missing variable by NAME, never by value. An entry is
never skipped because its credentials are unset: that would look healthy while
capturing nothing.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | clean shutdown after SIGINT/SIGTERM |
| 1 | a connection went fatal (a journal file could not be opened or written, or the journal ring overflowed) |
| 2 | startup error: bad arguments, config, selection or credentials, or `order_books` on with a Deribit connection that has no decimals |

A second SIGINT/SIGTERM during shutdown exits immediately with 130, so a slow
multi-connection shutdown never swallows a second Ctrl-C.

### Fatal stops everything

Any connection's fatal error stops every connection, and the log names which
one (`[<id>] capture failed, stopping every connection`). It is deliberate and
has no option: the common cause is a shared disk. The cost is that one
connection's bad journal directory or id now stops the others, which two
separate processes would not have done.

### Startup and shutdown order

Startup: every non-Kraken connection starts first, then the Kraken instrument
lookup (`AssetPairs`), then the Kraken connections. The lookup is a blocking
REST GET (10 s connect and 20 s transfer timeouts, so up to about 30 s), and run
first it would delay Deribit's capture behind a slow Kraken endpoint. It cannot
simply run after the Kraken connections start because it takes the same request
mutex as their token fetches.

Shutdown: a stop is requested on every connection first and only then are they
joined, so N connections wind down together instead of one after another. Joining
a connection also closes its journal, which waits for the journal thread to drain
the file. Only after every connection is joined do the books stop (below), so
nothing is still producing while their rings drain.

The journal file is `<id>-<connect_id>-<UTC timestamp>.journal`. The id is in
the name because two connections to one exchange would otherwise both write
`kraken-000001-...` and the second would truncate the first when started in the
same second. The exchange tag inside the file header is still `kraken` or
`deribit`, not the id.

A journal holds every symbol its connection subscribed to, interleaved in
arrival order. Anything reading one for a single symbol has to filter.

### What `connect_id` is

`connect_id` numbers the established connections of one `[[connections]]` entry.
It is 1 for the first connection the entry establishes and goes up by one on
every reconnect (a staleness timeout, a dropped socket, a sequence gap). Each
`connect_id` is its own journal file, and the file starts with a connect marker
record, so a reader and every sink learn that a fresh snapshot follows and any
book must reset.

- It is per connection, not global: two entries each have their own count, and
  both can be on `connect_id` 1 at once (the `<id>` in the file name is what
  tells their files apart).
- It restarts at 1 on every process run, which is why the file name also carries
  the UTC timestamp.
- It is not journal rotation. A file is never rotated for size or age; a new
  file happens only because a new connection was established.
- Sinks are told both ends: `OnConnect(connect_id, reason)` once the new file is
  usable and `OnDisconnect(connect_id)` once per connect, after its file is closed
  (`decisions/0004`). A book goes stale on the disconnect, not at the next connect.

## Threads and the journal

A connection's own thread (IXWebSocket's for Kraken, the client's for Deribit) does
no disk I/O and no parsing. In a running capture there are three kinds of thread:

| Thread | Count | Does |
|---|---|---|
| connection | one per `[[connections]]` entry | receives, stamps each frame once, copies it into the journal ring (and into the book ring when books are on) and returns |
| journal | one per connection | drains that connection's journal ring into its file |
| book | one for the process, only if `order_books` and at least one connection got books | polls every connection's book ring round-robin, parses and applies |

- **A full journal ring or a failed write is fatal**, through a fatal handler each
  client registers on its `CaptureSession`: it logs `journal failed: <reason>` and
  stops every connection, exit code 1. The ring is 65,536 events by default
  (`CaptureSession::Config::journal_ring_events`; no TOML key), sized for seconds
  of traffic. A journal write error is therefore reported asynchronously, by the
  handler, and not by the send path.
- **Closing a file is a barrier**: when a connection is joined or its socket is
  lost, the file is complete and readable once that returns.
- **Opening a file is synchronous.** A journal directory that cannot be created, or
  a header that cannot be written, fails the connect immediately and is fatal.
- The connection thread makes one heap allocation and one payload copy per frame
  per ring. That is accepted for now; `journal_replay` (`docs/modules/order-book.md`)
  is how to measure it.
- A test that needs a ring to overflow leaves the consumer unstarted
  (`Config::journal_start = false`) rather than racing it; see
  `docs/tasks/testing.md`.

## Order books

`order_books = true` (top level, default `false`) runs the golden books off the live
captures. It is off by default because a capture tool's job is journaling.

- **Wiring.** The `feed_handler` library does not depend on the book library. The
  capture binary links both and, through `CaptureConnection::AddSink` and the
  `BeforeStart` callback of `CaptureSet::StartAll`, gives every connection a ring and
  a sink (`src/book_adapter/book_wiring.cpp`). The callback runs just before each
  connection starts, which is when a Kraken connection's scale is known (after the
  `AssetPairs` lookup) and `AddSink` is still legal.
- **Kraken** builds its books with the connection's `depth` and the scale from the
  lookup: the finest decimals among its symbols. A symbol the lookup does not know
  is logged (`order books off for this connection, capture continues without
  them`) and that connection is captured without books; it is never a startup
  error and never a reason to stop.
- **Deribit** needs both `price_decimals` and `quantity_decimals` on the connection
  when `order_books` is on, or the process refuses to start (exit 2, naming the
  connection and the missing keys).
- The book thread starts after every connection has its ring, i.e. after
  `StartAll` returns. A Deribit connection starts before the `AssetPairs` lookup
  (up to about 30 s), so its ring buffers meanwhile; it is far from full at that
  rate.
- **Books never fail a capture.** They cannot latch fatal or change the exit code. A
  full book ring drops the frame, counts it and desyncs that connection's books
  until its next `connect_id`; a parse or apply error is counted and desyncs.
  `decisions/0008` has the policy.
- The ring is 65,536 events per connection (`BookService::Config::ring_events`, no
  config key yet).
- The books are only inspectable once the book thread has stopped; there is no
  live query surface yet.

### Summary lines

At exit, after the connections are joined and the books drained, the log has one
line per connection from the capture and, with books on, one from the books:

```
[kraken-btc-usd] captured N messages across N connect(s), N journal records, N watchdog-forced reconnect(s)
[kraken-btc-usd] books: N frames, N snapshots, N updates, N dropped, N parse errors, N apply errors, integrity issues: none, N book(s) at exit
```

The Deribit capture line also counts snapshots, incrementals and connection
attempts. On the books line, `dropped` is frames the ring refused, and anything other
than `none` in `integrity issues` (`gap`, `checksum_mismatch`, `crossed_book`,
`unknown_order`, `unknown_level`) means a book desynced. A healthy run has `0`
dropped, `0` parse and apply errors and `none`.

## Schema

`config/feed_handler.toml` is the committed default (it reproduces what the
binaries used to hardcode) and a test parses it, so it cannot rot.

| Key | Where | Meaning |
|---|---|---|
| `journal_dir`, `state_dir` | top level, optional | default `journal`, `state`, relative to the working directory |
| `order_books` | top level, optional | boolean, default `false`: run order books off the live captures. Off so a capture tool keeps only journaling until books have run for a while |
| `id` | connection | lowercase letters, digits, `_`, `-`, at most 48; names the journal files |
| `exchange` | connection | `kraken` or `deribit` |
| `env` | connection | `prod` or `testnet` (Kraken: `prod` only), required so a connection never silently picks one |
| `feed` | connection, optional | `level3` (Kraken) or `book` (Deribit): the only one each supports |
| `symbols` | connection | non-empty, no duplicates, characters `A-Za-z0-9_-./` only |
| `endpoint` | connection, optional | Kraken: a `ws(s)://` URL. Deribit: `host:port` |
| `api_key_env`, `api_secret_env` | connection | the NAMES of environment variables, never the credentials |
| `depth` | Kraken connection, optional | level3 subscribe depth: `10` (default), `100` or `1000`. Always sent explicitly on the subscribe, so the depth a book is built for and the depth subscribed cannot drift. Rejected on a Deribit connection |
| `price_decimals`, `quantity_decimals` | Deribit connection, optional | integer 0 to 15: decimal places of the integers a book works in, applied to every symbol on the connection, so use the finest decimals among its symbols. Required once `order_books` is on. Rejected on a Kraken connection, whose scale comes from the `AssetPairs` lookup |

`depth` and the decimals are parsed and validated here; what reads them to build
books is `src/book_adapter/book_wiring.cpp`, not the config layer. Kraken's `10`,
`100` and `1000` are as documented by Kraken; only `10` has been run live
(`exchanges/kraken.md`). `depth` is always sent, so the default `10` is not
implicit.

Validation is strict and reports the entry it failed on: unknown keys are
rejected (a `symbol` for `symbols` typo would otherwise capture nothing while
looking healthy), and the credential fields must look like variable names, so
pasting a secret there is refused rather than stored.

The symbol character set is a safety rule, not a style one: symbols are
spliced without escaping into a hand-built JSON string (Kraken) and a
SOH-delimited FIX field (Deribit). Loosening it means adding escaping first.

## Gotchas

- **Endpoint defaults exist only where this project has connected**: Kraken
  prod and Deribit testnet. Deribit prod requires an explicit `endpoint`; the
  Deribit production FIX host has not been verified here.
- **Kraken connections must be `env = "prod"`**, rejected when the config is
  loaded: the token call is a production REST call and there is no Kraken
  testnet REST endpoint to pair a different websocket with.
- **Kraken: at most 200 symbols per connection** (validated), and the
  subscribe is one message. The subscribe cost is depth-weighted (5 per symbol
  at depth 10 against a 200/s budget, `decisions/0004`), so a burst past about
  40 symbols would need pacing, which is not implemented. At the intended 5-10
  symbols it does not matter.
- **Kraken acknowledges a multi-symbol subscribe once per symbol.** The client
  logs each as `subscribed to level3 SYM (k/N)`; a rejected symbol does not close
  the connection, so `k < N` in the log is the only sign of a partial capture.
- **Deribit answers with one snapshot per symbol** and has no ack, so the log's
  `received a MarketDataSnapshotFullRefresh for SYM` lines are the check. What
  Deribit does when only one symbol of several is invalid is not confirmed.
- **All Kraken connections share one `RestClient`**, deliberately: it owns the
  nonce high-water mark, and its token fetches are serialized by a mutex
  because the nonce source and HTTP client under it are single-threaded.
- **Every log line past config load carries `[<id>]`**, which is how connections
  in one process are told apart.

## Adding an exchange

Everything exchange-specific is one enumerator, one data row and one small class;
nothing else names an exchange:

1. `config::Exchange` gains an enumerator, added to `kAllExchanges` and to the
   switch in `TraitsOf` (the compiler checks the switch). The name, the
   `--exchange` flag, the usage line and the config error text all follow.
2. `<exchange>/<exchange>_endpoints.h` defines an `ExchangeTraits` row: the name,
   the feed, the symbol cap, whether a testnet exists, how an endpoint is
   validated and which defaults have actually been connected to. The config
   validator reads the row; a test checks each row against its own rules.
3. A client with `Start()`, `RequestStop()`, `Join()` and `Fatal()` that journals
   through a `CaptureSession`. Its timed waits go through `StopSignal`
   (`stop_signal.h`), which owns the wakeup that ends them on a stop or a fatal
   error.
4. `<exchange>/<exchange>_capture.{h,cpp}`: a class deriving from
   `ClientCapture<Client>` that says how to build the client and what its
   `Summary()` reports, and a `Make<Exchange>Capture` factory. `ClientCapture`
   owns the shared teardown contract.
5. One `case` in `CaptureSet::Build`, plus a step in `StartAll` only if the
   exchange needs something done before its connections start.

Credentials need nothing for an exchange that authenticates with a key and a
secret: a config entry names its two variables and `ResolveCredentials` reads
them. An exchange that needs something else (a passphrase, or no credentials at
all) would also touch the config validator and `credentials.h`; a
`requires_credentials` field on its traits row is the smallest fit if that
comes up.
