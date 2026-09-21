---
aliases: [feed handler, feed-handler, capture, config, connections]
sources: [src/feed_handler/**, config/**]
decisions: [decisions/0001-feed-source-selection.md, decisions/0004-feed-handler-architecture.md]
---

# Feed handler

The capture side of the pipeline: one `feed_handler` binary connects to every
configured exchange, subscribes to the configured symbols, and journals every
inbound wire message verbatim. Which connections to open is a TOML file passed
with `--config`; the design rationale is in `decisions/0004`, and this is what
the file means and what to watch for.

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
| 1 | a connection went fatal (a journal file could not be opened or written) |
| 2 | startup error: bad arguments, config, selection or credentials |

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
joined, so N connections wind down together instead of one after another.

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
books is the order-book wiring, not the config layer.

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
